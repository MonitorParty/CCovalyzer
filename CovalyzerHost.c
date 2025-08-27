#include <assert.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <string.h> 
#include <stdbool.h> 
#include <sqlite3.h>
#include <errno.h>
#include <sys/wait.h> 
#include <stdlib.h>
#include <stdatomic.h>
#include <sys/stat.h>


#define C_GREEN   "\x1b[32;1m"
#define C_YELLOW  "\x1b[33;1m"
#define C_RED     "\x1b[31;1m"
#define C_RESET   "\x1b[0m"

#define PREF_OK   C_GREEN  "[+]" C_RESET " "
#define PREF_WARN C_YELLOW "[~]" C_RESET " "
#define PREF_ERR  C_RED    "[X]" C_RESET " "

#define DB_PATH "/home/stefan/db_test.db"
#define DB_PATH_QUEUE "/home/stefan/db_queue.db"

#define SHM_NAME "/covalyzer_shm"
#define SHM_SIZE (1024 * 1024 * 100) // 1 GB 
#define MAX_INPUT_SIZE (SHM_SIZE - sizeof(struct shm_state))

#define UNIT_TIMEOUT "10"
#define RSS_LIMIT_MB "2048"
#define EXIT_BUFFER 5
#define PROFRAW_PATTERN "cov-%m.profraw"  // %m allows llvm to merge from multiple processes
#define EXEC_TIMEOUT 10 

//implicit declaration of asprintf thingy
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1 
#endif 

typedef enum { SANE, INSANE } crash_mode_t;
typedef enum { STATEFUL, NON_STATEFUL, FUZZBENCH} state_mode_t;
typedef enum { QUEUE_ONLY, ALL_CASES } testset_mode_t;
typedef struct shm_state {
	volatile atomic_int ready; // set to one by host when data is ready 
	volatile atomic_int processed; // set to 1 by target when processing is done 
	volatile atomic_int target_ready;
	uint8_t modeset;			       //
	size_t size; 		// input size 
	uint8_t data[]; 	// flexible array of input data 
} shm_layout_t;


volatile sig_atomic_t child_exit_flag = 0;
volatile sig_atomic_t child_status = 0;

shm_layout_t *shm_ptr = NULL;
int shm_fd = -1;
sqlite3 *db = NULL;
sqlite3 *db_queue = NULL;
sqlite3_stmt *stmt = NULL;
sqlite3_stmt *stmt_restart = NULL;
sqlite3_stmt *stmt_queue = NULL;
pid_t target_pid;
uint8_t crashed = 0;
uint8_t run_dead = 0;
uint8_t include_queue = 1;

//Default vars 
crash_mode_t crash_mode = SANE;
state_mode_t state_mode = STATEFUL;
testset_mode_t testset_mode = QUEUE_ONLY;
size_t max_cases = 0;
char *instance_id; 
char path[256];

char **argv_glob;
size_t crashes = 0;
size_t restarts = 0;
size_t timeouts = 0;
size_t last_case;
size_t next_restart;
time_t last_execution = 0;
pid_t own_pid;


//time management 
time_t first_ts;
int current_bucket = -1;
int prev_bucket = -1;
char profraw_path[256];
//time constraint for buckets. 900 is default, which is 15 minutes. 
size_t bucket_time_limit = 900; 


//void set_env(){
//	if(state_mode == FUZZBENCH){
//		setenv("LLVM_PROFILE_FILE", profraw_path, 1);
//	}else{
//		setenv("LLVM_PROFILE_FILE", PROFRAW_PATTERN, 1);
//	}
//}


// TODO: maybe check if shm is empty or has residue from last run 
// HACK: test
void init_shm() {
	strcpy(path, "/ccov-");
	strcat(path, instance_id);
	shm_fd = shm_open(path, O_CREAT | O_RDWR, 0666);
	if (shm_fd == -1) {
		perror("shm_open");
		exit(1);
	}

	if (ftruncate(shm_fd, SHM_SIZE) == -1) {
		perror("ftruncate");
		exit(1);
	}

	shm_ptr = mmap(0, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
	if (shm_ptr == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}

	memset((void *)shm_ptr, 0, SHM_SIZE);
	shm_ptr->ready = 0;
	shm_ptr->processed = 1;
	shm_ptr->size = 0;
	shm_ptr->target_ready = 0;
	uint8_t modeset = 0; 
	if(crash_mode == INSANE){
		modeset = 1;
	}else{
		modeset = 0;
	}

	if(state_mode == NON_STATEFUL){
		modeset |= 2;
	}
	shm_ptr->modeset = modeset;
	fprintf(stderr, PREF_OK "SHM ready.\n");
}
void cleanup(){
	if(target_pid > 1){
		kill(target_pid, SIGTERM);
		if (waitpid(target_pid, NULL, 0) == -1 && errno != ECHILD) {
			perror("waitpid");
		}
	}

	
	if(stmt) sqlite3_finalize(stmt);
	if(db) sqlite3_close(db);
	if(shm_ptr) munmap(shm_ptr, SHM_SIZE);
	if (shm_fd != -1) close(shm_fd);
	fprintf(stderr, PREF_ERR "Cleanup called!\n");
	shm_unlink(path);
	
}

int get_current_bucket(time_t timestamp){
	return ( (timestamp - first_ts) / 900); }


//this method should copy the current cov profile to ./snapshots 
void backup_cov(){
	struct stat st = {0};
	// Create the snapshots directory if it does not exist
	if (stat("snapshots", &st) == -1) {
		if (mkdir("snapshots", 0755) != 0) {
			perror("mkdir");
			return;
		}
	}

	char dest[1024];
	snprintf(dest, sizeof(dest), "./snapshots/snap_%i.profraw", current_bucket);

	char *source = "cov-9526650416236694333_0.profraw";

	if(access(source, F_OK)){
		fprintf(stderr, PREF_WARN "Would have saved coverage snapshot, but nothing here...\n");
		return;
	}
	FILE *src = fopen(source, "rb");
	if(!src){
		perror("fopen");
		fprintf(stderr, PREF_ERR "Error while saving cov snapshot!\n");
		exit(1);
	}

	FILE *dst = fopen(dest, "wb");
	if(!dst){
		perror("fopen");
		fprintf(stderr, PREF_ERR "Error while saving cov snapshot!\n");
		exit(1);
	}

	char buf[8192];
	size_t n;
	while((n = fread(buf, 1, sizeof(buf), src)) > 0){
		if(fwrite(buf, 1, n, dst) != n) {
			perror("fwrite");
			fclose(src);
			fclose(dst);
			return;
		}
	}
	fclose(src);
	fclose(dst);
	fprintf(stderr, PREF_OK "Snapshot taken!\n");

}

void signal_handler(int sig){
	(void)sig;
	cleanup();
	fprintf(stderr, PREF_ERR "\nCovalyzerRunner shutting down! Signal:%i\n", sig);
	exit(0);
}

void sigchld_handler(int sig){
	(void)sig;
	int saved_errno = errno;
	pid_t pid;
	int status;
	crashes++;

	fprintf(stderr, PREF_WARN "Target signal received...");
	if(run_dead) goto skip;
	shm_ptr->target_ready = 0;
	while((pid = waitpid(-1, &status, WNOHANG)) > 0){
		if(pid == target_pid){
			child_status = status;
			child_exit_flag = 1;
			//target_pid = -1;
		}
	}
	fprintf(stderr, PREF_OK "Loop done");
skip:
	errno = saved_errno;
}

void term_handler(int sig, siginfo_t *si, void *u){
	(void)u;
	(void)sig;
	fprintf(stderr, PREF_ERR "signal_handler: got SIGTERM (si_pid =%d, si_uid=%d)\n", si? si->si_pid : -1, si  ? si->si_uid : -1);
	if(si->si_pid == own_pid){
		fprintf(stderr, PREF_ERR "got own kill...\n");
		return;
	}
	cleanup();
	exit(0);
}

void setup_term_handler(void){
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = term_handler;
	sa.sa_flags = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	if(sigaction(SIGTERM, &sa, NULL) == -1){
		perror("sigaction(SIGTERM)");
		exit(1);
	}
}

void init_sql(){
	int rc = sqlite3_open_v2(DB_PATH, &db, SQLITE_OPEN_READONLY, NULL);
	if (rc != SQLITE_OK){
		fprintf(stderr, PREF_ERR "Cannot open db: %s\n", sqlite3_errmsg(db));
		exit(1);
	}
	printf(PREF_OK "DB opened.");
	int rc_queue;
	if(include_queue){
		rc_queue = sqlite3_open_v2(DB_PATH_QUEUE, &db_queue, SQLITE_OPEN_READONLY, NULL);
		if (rc_queue != SQLITE_OK){
			fprintf(stderr, PREF_ERR "Cannot open db_queue: %s\n", sqlite3_errmsg(db));
			exit(1);
		}
		printf(PREF_OK "queue DB opened.");


	}
	const char *query = NULL;
	if (testset_mode == QUEUE_ONLY){
		if(max_cases){
			asprintf(&query, "SELECT testcases.value, testcases.id, testcases.timestamp FROM testcases JOIN queue ON queue.id = testcases.id WHERE testcases.id < %zu;", max_cases);
		}else{
			asprintf(&query, "SELECT testcases.value, testcases.id, testcases.timestamp FROM testcases JOIN queue ON queue.id = testcases.id;");
		}
	}else if( testset_mode == ALL_CASES){
		if(max_cases){
			asprintf(&query, "SELECT testcases.value, testcases.id, testcases.timestamp FROM testcases WHERE testcases.id < %zu;", max_cases);

		}else{
			query = "SELECT value, id, timestamp FROM testcases;";
		}
	}else{
		fprintf(stderr, "%sUnsupported testset mode!\n", PREF_ERR);
	}

	const char *restart_query = "SELECT id FROM  restarts ORDER BY id ASC;";
	

	rc = sqlite3_prepare_v2(db, query, -1, &stmt, NULL);
	if(rc != SQLITE_OK){
		fprintf(stderr, "[!] Failed to prepare query: %s\n", sqlite3_errmsg(db));
		exit(1);
	}

	rc = sqlite3_prepare_v2(db, restart_query, -1, &stmt_restart, NULL);
	if(rc != SQLITE_OK){
		fprintf(stderr, PREF_ERR "Failed to prepare query for restarts: %s\n", sqlite3_errmsg(db));
		exit(1);
	}
	next_restart = -1;

	if(!include_queue) return;
	const char* queue_query = "SELECT id, data FROM queue_cases;";
	rc_queue = sqlite3_prepare_v2(db_queue, queue_query, -1, &stmt_queue, NULL);
	if(rc_queue != SQLITE_OK){
		fprintf(stderr, PREF_ERR "Failed to load initial queue settings!");
	}

}

void update_restarts(){
	//TODO: What if no more restarts? 
	if(sqlite3_step(stmt_restart) == SQLITE_ROW){
		next_restart = sqlite3_column_int(stmt_restart, 0);
	}
}

pid_t start_target_process(const char *target_path){
	pid_t pid = fork();
	if(pid < 0){
		perror("fork");
		return -1;
	}
	if(pid == 0){
		//child 
		//set LLVM env vars needed 

		sigset_t empty;
		sigemptyset(&empty);
		sigprocmask(SIG_SETMASK, &empty, NULL);
		if(setsid() == -1){
			perror("setsid");
		}

		setenv("LLVM_PROFILE_FILE", PROFRAW_PATTERN, 1);
		setenv("CCOV_INSTANCE", instance_id, 1);
		setenv("HB_DEBUG", "0", 1);
		shm_ptr->target_ready = 0;
		execl(target_path,
				target_path,
				"-merge=1",
				"-dump_coverage=1",
				"-artifact_prefix=./crashes/",
				"-timeout=" UNIT_TIMEOUT,
				"-rss_limit_mb=" RSS_LIMIT_MB,
				NULL);
		perror("execl");
		return -1;
		exit(1);
		
	}else if(pid > 0){
		child_exit_flag = 0;
		target_pid = pid;
		crashed = 0;
		shm_ptr->ready = 0;
		shm_ptr->processed = 1;
		shm_ptr->size = 0;

		fprintf(stderr, PREF_ERR "Current pid: %i", target_pid);
		return pid;
	}
	return -1;
}

void restart_target() {
    sigset_t set, oldset;
    restarts++;

    // Block SIGCHLD so handler can't run while we're restarting
    sigemptyset(&set);
    sigaddset(&set, SIGCHLD);
    sigaddset(&set, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &set, &oldset) == -1) {
        perror("sigprocmask (block)");
        return;
    }


    //check if crash happened
    if(child_exit_flag){
	    child_exit_flag = 0;
    }

    // Snapshot PID before handler can change it
    pid_t pid = target_pid;

    if(state_mode == NON_STATEFUL){
	    goto skip_kill;
    }
    if (pid > 1) {
        if (kill(pid, SIGTERM) == -1 && errno != ESRCH) { //11 bc. TERM does not want to work :()
            perror("kill");
        }
	fprintf(stderr, PREF_WARN "Sent SIGTERM to pid=%d\n", pid);
    }else{
	    fprintf(stderr, PREF_WARN "NOT killing target pid=%d\n", pid);
    }
skip_kill:
    ;
    // Wait for old child to be reaped if it's still around
    int status = 0;
    if (pid > 1) {
        if (waitpid(pid, &status, 0) == -1 && errno != ECHILD) {
            perror("waitpid");
        }
    }

    shm_ptr->target_ready = 0;
    shm_ptr->ready = 0;
    shm_ptr->processed = 1;

    // Reset crash flags, start fresh target
    //child_exit_flag = 0;
    target_pid = start_target_process(argv_glob[1]);

    sigpending(&set);  // Check pending signals
    if (sigismember(&set, SIGTERM)) {
        struct timespec ts = {0};
        sigtimedwait(&set, NULL, &ts);  // Clear pending SIGTERM
    }
    // Restore signal mask (unblock SIGCHLD)
    if (sigprocmask(SIG_SETMASK, &oldset, NULL) == -1) {
        perror("sigprocmask (restore)");
    }
    fflush(stderr);

        }

bool send_testcase(const void *data, size_t len){
	if (len > MAX_INPUT_SIZE){
		fprintf(stderr, "[!] Input too large (%zu bytes), skipping.\n", len);
		return false;
	}
	//wait if target did not finish prev run, but it shouldnt wait here 
	while(!shm_ptr->processed && !child_exit_flag) usleep(1000);
	if(child_exit_flag){
		fprintf(stderr, PREF_WARN "Restarting inside send_testcase():2...\n");
		restart_target();
	}

	//set last exec timer if needed
	if(last_execution == 0){
		last_execution = time(NULL);
	}

	while(!shm_ptr->target_ready && !((time(NULL) - last_execution) > EXEC_TIMEOUT)) usleep(1000);

	if((time(NULL) - last_execution) > EXEC_TIMEOUT){
		//we are in timeout corner :( 
		fprintf(stderr, PREF_WARN "Target timed out, restarting...\n");
		timeouts++;
		restart_target();
	}

	memcpy(shm_ptr->data, data, len);
	shm_ptr->size = len;
	shm_ptr->ready = 1;
	shm_ptr->processed = 0;

	//set last_execution time 
	last_execution = time(NULL);
	
	while(!shm_ptr->processed && !child_exit_flag && !((time(NULL) - last_execution) > EXEC_TIMEOUT)) usleep(1000);
	if(child_exit_flag){
		fprintf(stderr, PREF_WARN "Restarting inside send_testcase():3...Starting %s\n", argv_glob[1]);
		
		restart_target();
	}else if((time(NULL) - last_execution) > EXEC_TIMEOUT){
		fprintf(stderr, PREF_WARN "Timeout detected, restarting target...\n");
		restart_target();
	}
	return true;
}

void parse_args(int argc){
	// CovalyzerHost <target_bin> <crash> <state> <test> (maxTC)
	char *usage = PREF_ERR "Usage: %s <target_bin> <crashmode> <statemode> <testmode> <instance_id/experiment name> (maxTC)\nHint: expecting main db (link) @ /home/stefan/{db_test.db,db_queue.db} (main + queue db)\nHint: This is not bad coding, this is O(1) coding efficiency.\n";
	if(argc < 7){
		fprintf(stderr, usage, argv_glob[0]);
		exit(1);
	}else if (argc >= 7){
		if(strcmp(argv_glob[2], "sane") == 0){
			crash_mode = SANE;
			fprintf(stderr, PREF_OK "Crash mode: sane\n");
		}else if(strcmp(argv_glob[2], "insane") == 0){
			crash_mode = INSANE;
			fprintf(stderr, PREF_OK "Crash mode: insane\n");
		}else{
			fprintf(stderr, PREF_ERR "Unknown crashmode: %s\nPick from: sane insane\n", argv_glob[2]);
			exit(1);
		}
		if(strcmp(argv_glob[3], "stateful") == 0){
			state_mode = STATEFUL;
			fprintf(stderr, PREF_OK "State mode: stateful\n");
		}else if(strcmp(argv_glob[3], "stateless") == 0){
			state_mode = NON_STATEFUL;
			fprintf(stderr, PREF_OK "State mode: stateless\n");
		}else if(strcmp(argv_glob[3], "fuzzbench") == 0){
			state_mode = FUZZBENCH;
			fprintf(stderr, PREF_OK "State mode: fuzzbench\n");
		}else{
			fprintf(stderr, PREF_ERR "Unknown state mode: %s\nPick from: stateful stateless fuzzbench\n", argv_glob[3]);
			exit(1);
		}

		if(strcmp(argv_glob[4], "queue") == 0){
			testset_mode = QUEUE_ONLY;
			fprintf(stderr, PREF_OK "Test mode: queue only\n");
		}else if(strcmp(argv_glob[4], "all") == 0){
			testset_mode = ALL_CASES;
			fprintf(stderr, PREF_OK "Test mode: all cases\n");
		}else{
			fprintf(stderr, PREF_ERR "Unknown testset mode: %s\nPick from: queue all\n", argv_glob[4]);
			exit(1);
		}

		//yes we just hope no instance id is bigger than 256 ... trust 
		instance_id = argv_glob[5];
		fprintf(stderr, PREF_OK "Set instance id to: %s\n", instance_id);

		if(argc >= 7){
			max_cases = atoi(argv_glob[6]);
			fprintf(stderr, PREF_WARN "Limited to %zu cases!\n", max_cases);
		}


	}
}




//void restart_target(){
//	//kill -> wait for signal -> spawn
//	fprintf(stderr, PREF_WARN "Killing the child (intentional)...\n");
//	if(target_pid > -1){
//		kill(target_pid, SIGTERM);
//	}
//	int status;
//	waitpid(target_pid, &status, 0);
//	//while(!child_exit_flag) usleep(1000);
//	fprintf(stderr, PREF_OK "Target dead. Respawning...\n");
//	start_target_process(argv_glob[1]);
//	fprintf(stderr, PREF_OK "Target respawned...\n");
//	restarts++;
//}


void check_bucket_state(time_t time){
	int current_new = ( (time - first_ts) / 900);
	if(current_new != current_bucket || current_bucket == -1){
		//we are in a new window
		//setup dirs, switch profraw path, merge ...
		fprintf(stderr, "\n===================\n"PREF_WARN "New bucket. New bucket ID: %03d\n", current_new);

		//char dir[64];
		//snprintf(dir, sizeof(dir), "bucket_%03d", current_new);
		//mkdir(dir, 0777);
		
		prev_bucket = current_bucket;
		current_bucket = current_new;

		//snprintf(profraw_path, sizeof(profraw_path), "bucket_%03d/cov-%%m.profraw", current_bucket);
		//setenv("LLVM_PROFILE_FILE", profraw_path, 1);

		//merge logic
		if(prev_bucket == -1){
			//dont merge
		}else{
			//merge prev
		}
	}
			
}




uint8_t is_new_bucket(time_t timestamp){
	int current_new = ( (timestamp - first_ts) / 900);
	if (current_new != current_bucket) return true;
	return false;
}

void set_new_bucket(time_t timestamp){
	prev_bucket = current_bucket;
	current_bucket = ((timestamp - first_ts) / 900);
}



int main(int argc, char **argv){
	
	setbuf(stderr, NULL);
	argv_glob = argv;

	parse_args(argc);
	own_pid = getpid();


	//our special needs child
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigchld_handler;
	sa.sa_flags  = SA_RESTART | SA_NOCLDSTOP;
	sigemptyset(&sa.sa_mask);
	if(sigaction(SIGCHLD, &sa, NULL) == -1){
		perror("sigaction(SIGCHLD)");
		exit(1);
	}


	//es reicht die scheiße wird jetzt verboten anders kann man damit auch nicht umgehen WOHER DIESE RANDOM SIGTERMS???????? SCHLIMMER ALS MEINE EX MIT DIESEN MIXED SIGNALS DIE AUS DEM NIX SPAWNEN
	sigset_t blockSet, savedSigMask;
	sigemptyset(&blockSet); /* initialize empty */
	sigaddset(&blockSet, SIGTERM);
	sigprocmask(SIG_BLOCK, &blockSet, &savedSigMask);

// ──────────────────────────────────────────────────────────────────────

	init_shm();
	init_sql();
	start_target_process(argv[1]);
	fprintf(stderr, "[+] CovalyzerRunner initialized. Sending spying pigeons containing test cases...\n");

//          ╭─────────────────────────────────────────────────────────╮
//          │                       Queue-runs                        │
//          ╰─────────────────────────────────────────────────────────╯

	//queue-runs
	fprintf(stderr, PREF_OK "Executing seeds...\n");
	while(sqlite3_step(stmt_queue) == SQLITE_ROW){
		
		//first, fetch data such that it is available for all thingies
		const void *blob = sqlite3_column_blob(stmt_queue, 1);
		int blob_size = sqlite3_column_bytes(stmt_queue, 1);
		int current_id = sqlite3_column_int(stmt_queue, 0);

							
		//check if target should be restarted
		if(child_exit_flag || state_mode == NON_STATEFUL){
			fprintf(stderr, PREF_WARN "Restart incoming... Last executed id: [%zu]\n", last_case);
			if(child_exit_flag){
				fprintf(stderr, PREF_ERR "Restart bc. target crash");
				//start_target_process(argv[1]);
				restart_target();
			}else{
				//restart needed 
				restart_target();
			}
		}

		last_case = current_id;

		if(blob && blob_size > 0){
			if(send_testcase(blob, (size_t)blob_size)){
				double progress = (max_cases > 0) ? ((double)current_id/ 1075) * 100.0 : 0.0;
				if(state_mode == FUZZBENCH){
					fprintf(stderr, "\r" PREF_OK "[SEEDS] Progress: [%.1f%%] | Crashes: [%zu] | Restarts: [%zu] | Bucket: [%i] | Executed seed ID %d (%d bytes)", progress, crashes, restarts, current_bucket, current_id, blob_size);
					fflush(stderr);	
				}else{
					fprintf(stderr, "\r" PREF_OK "Progress: [%.1f%%] | Crashes: [%zu] | Restarts: [%zu] | Executed seed ID %d (%d bytes)", progress, crashes, restarts, current_id, blob_size);
				}
				//save exec time 
			}
		}
	}

	fprintf(stderr, PREF_OK "\n\nSeed runs finished...\n");


//          ╭─────────────────────────────────────────────────────────╮
//          │                        Real runs                        │
//          ╰─────────────────────────────────────────────────────────╯

	size_t count = 0;
	size_t last_case = 0;
	while(sqlite3_step(stmt) == SQLITE_ROW && count <= max_cases){
		
		//first, fetch data such that it is available for all thingies
		const void *blob = sqlite3_column_blob(stmt, 0);
		int blob_size = sqlite3_column_bytes(stmt, 0);
		int current_id = sqlite3_column_int(stmt, 1);
		const char *time_str = (const char *)sqlite3_column_text(stmt, 2);
		time_t timestamp = (time_t)atol(time_str); //(time_t) sqlite3_column_int64(stmt, 2);

		if(count == 0 ){
			first_ts = timestamp;
			set_new_bucket(timestamp);
			fprintf(stderr, PREF_WARN "First bucket id is: %i\n", current_bucket);
		}
					
		//check if target should be restarted
		if(((last_case == next_restart) && state_mode != FUZZBENCH) || (is_new_bucket(timestamp) && state_mode == FUZZBENCH) || child_exit_flag || state_mode == NON_STATEFUL){
			fprintf(stderr, PREF_WARN "Restart incoming... Last executed id: [%zu]\n", last_case);
			if(is_new_bucket(timestamp) && state_mode == FUZZBENCH){
				fprintf(stderr, PREF_WARN "Restart bc. new time bucket\n");
				backup_cov();
				set_new_bucket(timestamp);
				restart_target();
			}else if(last_case == next_restart){
				fprintf(stderr, PREF_WARN "Restart was planned by database\n");
				update_restarts();
				restart_target();
			}else if(child_exit_flag){
				fprintf(stderr, PREF_ERR "Restart bc. target crash");
				//start_target_process(argv[1]);
				restart_target();
			}else{
				//restart needed 
				restart_target();
			}
		}

		//check if we have to copy 
		if(is_new_bucket(timestamp) ){
			backup_cov();
			if(state_mode != FUZZBENCH){
				set_new_bucket(timestamp);
			}
		}


		//send testcase, print progress...
		last_case = current_id;
		if(blob && blob_size > 0){
			if(send_testcase(blob, (size_t)blob_size)){
				double progress = (max_cases > 0) ? ((double)current_id/ max_cases) * 100.0 : 0.0;
				if(state_mode == FUZZBENCH){
					fprintf(stderr, "\r" PREF_OK "Progress: [%.1f%%] | Crashes: [%zu] | Restarts: [%zu] | Bucket: [%i] | Executed testcase ID %d (%d bytes)", progress, crashes, restarts, current_bucket, current_id, blob_size);
					fflush(stderr);	
				}else{
					fprintf(stderr, "\r" PREF_OK "Progress: [%.1f%%] | Crashes: [%zu] | Restarts: [%zu] | Executed testcase ID %d (%d bytes)", progress, crashes, restarts, current_id, blob_size);
				}
				//save exec time 
			}
		}
		count++;
	}
	fprintf(stderr, "\n" PREF_OK "Executed all cases, KILLING OUR CHILD...\n");
	run_dead = 1;
	fprintf(stderr, PREF_OK "Done.\n");
	cleanup();
	return 0;
}
