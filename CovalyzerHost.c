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
#define FIRST_TC_TIMESTAMP 1754064325

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
	uint8_t modeset;		       //
	size_t size; 	// input size 
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

sqlite3_stmt *stmt_seed_files = NULL;




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
size_t total_execs;
pid_t own_pid;


//time management 
time_t first_ts;
int current_bucket = -1;
int prev_bucket = -1;
char profraw_path[256];
//time constraint for buckets. 900 is default, which is 15 minutes. 
size_t bucket_time_limit = 900; 

long long max_timestamp = 0;
int max_bucket = -1;


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


void query_limit_metadata() {
	fprintf(stderr, PREF_WARN "Starting max timestamp shit...\n");
	fflush(stderr);

	sqlite3 *tmp_db;
	sqlite3_stmt *tmp_stmt;
	int rc = sqlite3_open_v2(DB_PATH, &tmp_db, SQLITE_OPEN_READONLY, NULL);
	if (rc != SQLITE_OK) {
		fprintf(stderr, PREF_ERR "Cannot open db for timestamp limit: %s\n", sqlite3_errmsg(tmp_db));
		exit(1);
	}

	const char *query = "SELECT timestamp FROM testcases WHERE id = ?";
	rc = sqlite3_prepare_v2(tmp_db, query, -1, &tmp_stmt, NULL);
	if (rc != SQLITE_OK) {
		fprintf(stderr, PREF_ERR "Failed to prepare timestamp query: %s\n", sqlite3_errmsg(tmp_db));
		exit(1);
	}

	sqlite3_bind_int(tmp_stmt, 1, max_cases);

	if (sqlite3_step(tmp_stmt) == SQLITE_ROW) {
		max_timestamp = sqlite3_column_int64(tmp_stmt, 0);
		fprintf(stderr, PREF_OK "Found timestamp limit for testcase %zu: %lld\n", max_cases, max_timestamp);

		if (state_mode == FUZZBENCH) {
			max_bucket = (max_timestamp - FIRST_TC_TIMESTAMP) / bucket_time_limit;
			fprintf(stderr, PREF_OK "Calculated max bucket: %d\n", max_bucket);
		}
	} else {
		fprintf(stderr, PREF_WARN "Could not find a testcase at limit %zu. Disabling limit.\n", max_cases);
		max_cases = 0;
		max_timestamp = 0;
	}

	sqlite3_finalize(tmp_stmt);
	sqlite3_close(tmp_db);

}

//This method gets called iff: QUEUE && FUZZBENCH
//It differs from init_sql() by only fetching chunks of data, so a query gets constructed for the current bucket, and if 
//bucket is done, the query has to be evaluated again 
void init_sql_fuzzbench_buckets(){
	int rc = sqlite3_open_v2(DB_PATH, &db, SQLITE_OPEN_READONLY, NULL);
	if(rc != SQLITE_OK){
		fprintf(stderr, PREF_ERR "Cannot open db: %s\n", sqlite3_errmsg(db));
		exit(1);
	}
	fprintf(stderr, PREF_OK "DB opened.\n");
	sqlite3_exec(db, "PRAGMA temp_store = MEMORY;", NULL, NULL, NULL);
	if(current_bucket == -1){
		current_bucket = 0;
	}
	size_t new_ts_start = FIRST_TC_TIMESTAMP + (current_bucket * bucket_time_limit);
	size_t new_ts_end = new_ts_start + (bucket_time_limit - 1);

	fprintf(stderr, PREF_WARN "\nUpdating query for new bucket, this might take some time depending on your DB optimiziation...\n");
	//fflush(stderr);

	//mitigate problem of too many data being fetched in last interval
	if(max_timestamp > 0 && new_ts_end > max_timestamp){
		new_ts_end = max_timestamp;
	}



	char *queue_sorted_query;
	if(testset_mode == ALL_CASES){

		asprintf(&queue_sorted_query, "SELECT value, id, timestamp, size FROM (SELECT testcases.id AS id, testcases.value AS value, testcases.timestamp AS timestamp, testcases.size AS size, 'testcase' as source FROM testcases WHERE testcases.timestamp BETWEEN %zu AND %zu UNION ALL SELECT crashes_hangs.id AS id, crashes_hangs.data AS value, crashes_hangs.time AS timestamp, crashes_hangs.size AS size, 'crash_hang' AS source FROM crashes_hangs WHERE crashes_hangs.time BETWEEN %zu AND  %zu ) ORDER BY size ASC;"   , FIRST_TC_TIMESTAMP, new_ts_end, FIRST_TC_TIMESTAMP, new_ts_end);
	}else{

		asprintf(&queue_sorted_query, "SELECT value, id, timestamp, size FROM (SELECT testcases.id AS id, testcases.value AS value, testcases.timestamp AS timestamp, testcases.size AS size, 'testcase' as source FROM testcases JOIN queue ON testcases.id = queue.id WHERE queue.timestamp BETWEEN  %zu AND %zu UNION ALL SELECT crashes_hangs.id AS id, crashes_hangs.data AS value, crashes_hangs.time AS timestamp, crashes_hangs.size AS size, 'crash_hang' AS source FROM crashes_hangs WHERE crashes_hangs.time BETWEEN %zu AND  %zu ) ORDER BY size ASC;"   , FIRST_TC_TIMESTAMP, new_ts_end, FIRST_TC_TIMESTAMP, new_ts_end);
	}
	rc = sqlite3_prepare_v2(db, queue_sorted_query, -1, &stmt, NULL);
	free(queue_sorted_query);
	if(rc != SQLITE_OK){
		fprintf(stderr, PREF_ERR "Cannot open db: %s\n", sqlite3_errmsg(db));
		exit(1);
	}
	int rc_queue;
	if(include_queue){
		rc_queue = sqlite3_open_v2(DB_PATH_QUEUE, &db_queue, SQLITE_OPEN_READONLY, NULL);
		if (rc_queue != SQLITE_OK){
			fprintf(stderr, PREF_ERR "Cannot open db_queue: %s\n", sqlite3_errmsg(db_queue));
			exit(1);
		}
		printf(PREF_OK "queue DB opened.");
	}




	const char* queue_query = "SELECT id, data FROM queue_cases;";
	rc_queue = sqlite3_prepare_v2(db_queue, queue_query, -1, &stmt_queue, NULL);
	if(rc_queue != SQLITE_OK){
		fprintf(stderr, PREF_ERR "Failed to load initial queue settings!");
	}

	//this should be enough. We are selecting each TC, which is also listed in queue, plus all other seeds (for first iteration).
}

void update_query_new_bucket(){
	char *queue_sorted_query;
	size_t new_ts_start = FIRST_TC_TIMESTAMP + (current_bucket * bucket_time_limit);
	size_t new_ts_end = new_ts_start + (bucket_time_limit - 1);

	fprintf(stderr, PREF_WARN "Updating query for new bucket, this might take some time depending on your DB optimiziation...\n");
	fflush(stderr);

	//mitigate problem of too many data being fetched in last interval
	if(max_timestamp > 0 && new_ts_end > max_timestamp){
		new_ts_end = max_timestamp;
	}
	if(testset_mode == ALL_CASES){

		asprintf(&queue_sorted_query, "SELECT value, id, timestamp, size FROM (SELECT testcases.id AS id, testcases.value AS value, testcases.timestamp AS timestamp, testcases.size AS size, 'testcase' as source FROM testcases WHERE testcases.timestamp BETWEEN %zu AND %zu UNION ALL SELECT crashes_hangs.id AS id, crashes_hangs.data AS value, crashes_hangs.time AS timestamp, crashes_hangs.size AS size, 'crash_hang' AS source FROM crashes_hangs WHERE crashes_hangs.time BETWEEN %zu AND  %zu ) ORDER BY size ASC;", new_ts_start, new_ts_end, new_ts_start, new_ts_end);
	}else{

		asprintf(&queue_sorted_query, "SELECT value, id, timestamp, size FROM (SELECT testcases.id AS id, testcases.value AS value, testcases.timestamp AS timestamp, testcases.size AS size, 'testcase' as source FROM testcases JOIN queue ON testcases.id = queue.id WHERE queue.timestamp BETWEEN  %zu AND %zu UNION ALL SELECT crashes_hangs.id AS id, crashes_hangs.data AS value, crashes_hangs.time AS timestamp, crashes_hangs.size AS size, 'crash_hang' AS source FROM crashes_hangs WHERE crashes_hangs.time BETWEEN %zu AND  %zu ) ORDER BY size ASC;", new_ts_start, new_ts_end, new_ts_start, new_ts_end);
	}
	sqlite3_finalize(stmt);
	int rc = sqlite3_prepare_v2(db, queue_sorted_query, -1, &stmt, NULL);
	free(queue_sorted_query);
	if(rc != SQLITE_OK){
		fprintf(stderr, PREF_ERR "Cannot open db: %s\n", sqlite3_errmsg(db));
		exit(1);
	}
	fprintf(stderr, PREF_OK "Changed db bucket!\n");

}
void init_sql(){
	int rc = sqlite3_open_v2(DB_PATH, &db, SQLITE_OPEN_READONLY, NULL);
	if (rc != SQLITE_OK){
		fprintf(stderr, PREF_ERR "Cannot open db: %s\n", sqlite3_errmsg(db));
		exit(1);
	}
	printf(PREF_OK "queue DB opened.");
	sqlite3_exec(db, "PRAGMA temp_store = MEMORY;", NULL, NULL, NULL);
	printf(PREF_OK "DB opened.");
	char *query = NULL;
		char *restart_query = "SELECT id FROM  restarts ORDER BY id ASC;";
	if (testset_mode == QUEUE_ONLY){
		if(max_cases){
			asprintf(&query, "SELECT value, id, timestamp, size FROM (SELECT testcases.id AS id, testcases.value AS value, testcases.timestamp AS timestamp, testcases.size AS size, 'testcase' as source FROM testcases JOIN queue ON testcases.id = queue.id WHERE queue.timestamp BETWEEN  %zu AND %zu UNION ALL SELECT crashes_hangs.id AS id, crashes_hangs.data AS value, crashes_hangs.time AS timestamp, crashes_hangs.size AS size, 'crash_hang' AS source FROM crashes_hangs WHERE crashes_hangs.time BETWEEN %zu AND  %zu ) ORDER BY timestamp ASC;", 0, max_timestamp, 0, max_timestamp );
			//asprintf(&query, "SELECT testcases.value, testcases.id, testcases.timestamp FROM testcases JOIN queue ON queue.id = testcases.id WHERE testcases.id < %zu;", max_cases);
		}else{
			//FIXME: This is old.
			asprintf(&query, "SELECT testcases.value, testcases.id, testcases.timestamp FROM testcases JOIN queue ON queue.id = testcases.id;");
		}
	}else if( testset_mode == ALL_CASES){
		if(max_cases){

			asprintf(&query, "SELECT value, id, timestamp, size FROM (SELECT testcases.id AS id, testcases.value AS value, testcases.timestamp AS timestamp, testcases.size AS size, 'testcase' as source FROM testcases UNION ALL SELECT crashes_hangs.id AS id, crashes_hangs.data AS value, crashes_hangs.time AS timestamp, crashes_hangs.size AS size, 'crash_hang' AS source FROM crashes_hangs WHERE crashes_hangs.time BETWEEN %zu AND  %zu ) ORDER BY timestamp ASC;", 0, max_timestamp, 0, max_timestamp);
			//asprintf(&query, "SELECT testcases.value, testcases.id, testcases.timestamp FROM testcases WHERE testcases.id < %zu;", max_cases);

		}else{
			//FIXME: This is old
			query = "SELECT value, id, timestamp FROM testcases;";
		}
	}else{
		fprintf(stderr, "%sUnsupported testset mode!\n", PREF_ERR);
	}


	rc = sqlite3_prepare_v2(db, query, -1, &stmt, NULL);
	if(query) free(query);
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

	int rc_queue;
		rc_queue = sqlite3_open_v2(DB_PATH_QUEUE, &db_queue, SQLITE_OPEN_READONLY, NULL);
		if (rc_queue != SQLITE_OK){
			fprintf(stderr, PREF_ERR "Cannot open db_queue: %s\n", sqlite3_errmsg(db_queue));
			exit(1);
		}
		printf(PREF_OK "queue DB opened.");




	const char* queue_query = "SELECT id, data FROM queue_cases;";
	rc_queue = sqlite3_prepare_v2(db_queue, queue_query, -1, &stmt_queue, NULL);
	if(rc_queue != SQLITE_OK){
		fprintf(stderr, PREF_ERR "Failed to load initial queue settings!");
	}



}

void update_restarts(){
	//TODO: What if no more restarts? 
	//HACK: this seems to work somehow 
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
				"-timeout="
				UNIT_TIMEOUT,
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

	//FIX: this was not nice....
	//if(state_mode == NON_STATEFUL){
	//        goto skip_kill;
	//}
	if (pid > 1) {
		if (kill(pid, SIGTERM) == -1 && errno != ESRCH) { //11 bc. TERM does not want to work :() 
			perror("kill");
		}
		fprintf(stderr, PREF_WARN "Sent SIGTERM to pid=%d\n", pid);
	}else{
		fprintf(stderr, PREF_WARN "NOT killing target pid=%d\n", pid);
	}
	//skip_kill:
	//    ;
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

	crashes--;
}

bool send_testcase(const void *data, size_t len){
	if (len > MAX_INPUT_SIZE){
		fprintf(stderr, "[!] Input too large (%zu bytes), skipping.\n", len);
		return false;
	}
	//wait if target did not finish prev run, but it shouldnt wait here 
	while(!shm_ptr->processed && !child_exit_flag) usleep(1000);
	if(child_exit_flag){
		fprintf(stderr, PREF_WARN "Target crashed on TC %zu (or the one before), restarting before sending new one...\n", last_case);
		restart_target();
	}

	//set last exec timer if needed
	if(last_execution == 0){
		last_execution = time(NULL);
	}

	while(!shm_ptr->target_ready && !((time(NULL) - last_execution) > EXEC_TIMEOUT)) usleep(1000);

	if(((time(NULL) - last_execution) > EXEC_TIMEOUT) && !shm_ptr->processed){
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
		//this should be a crash branch I guess, since we just sent the TC and child exit is set
		fprintf(stderr, PREF_WARN "Target crashed on TC %zu, restarting...\n", last_case);

		restart_target();
		crashes++;
	}else if((time(NULL) - last_execution) > EXEC_TIMEOUT){
		fprintf(stderr, PREF_WARN "Timeout detected, restarting target...\n");
		restart_target();
		timeouts++;
	}
	return true;
}

void parse_args(int argc){
	// CovalyzerHost <target_bin> <crash> <state> <test> (maxTC)
	char *usage = PREF_ERR "Usage: %s <target_bin> <crashmode> <statemode> <testmode> <instance_id/experiment name> (maxTC) (bucket_time_limit; default 900=15min)\nHint: expecting main db (link) @ /home/stefan/{db_test.db,db_queue.db} (main + queue db)\nHint: This is not bad coding, this is O(1) coding efficiency.\n";
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
		if(argc >= 8){
			bucket_time_limit = atoi(argv_glob[7]);
			fprintf(stderr, PREF_WARN "Set time limit for buckets to %zu seconds!\n", bucket_time_limit);
		}



	}
}

void write_exit_log(){
	FILE *fp = fopen("result.txt", "w");
	if (fp == NULL) {
		perror("Error opening file");
		return ;
	}

	fprintf(fp, "===========CCOV STATS============\n");
	fprintf(fp, "Instance id: %s\n", instance_id);
	fprintf(fp, "total_execs: %zu\n", total_execs);
	fprintf(fp, "crashes: %zu\n", crashes);
	fprintf(fp, "timeouts: %zu\n", timeouts);
	fprintf(fp, "restarts: %zu\n", restarts);
	fprintf(fp, "limit: %zu\n", max_cases);
	fprintf(fp, "last bucket id: %i\n", current_bucket);
	fprintf(fp, "bucket size limit: %zu\n", bucket_time_limit);


	fclose(fp);

	fprintf(stderr, PREF_OK "Information File written successfully.\n");
	return ;
}


//void restart_target(){
//	//kill -> wait for signal -> spawn
//	fprintf(stderr, PREF_WARN "Killing the child (intentional)...");
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

uint8_t is_new_bucket(time_t timestamp){
	int current_new = ( (timestamp - first_ts) / bucket_time_limit);
	if (current_new != current_bucket) return true;
	return false;
}

void set_new_bucket(time_t timestamp){
	prev_bucket = current_bucket;
	current_bucket = ((timestamp - first_ts) / bucket_time_limit);
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
	if(max_cases > 0){
		query_limit_metadata();
	}

	if(state_mode == FUZZBENCH){
		init_sql_fuzzbench_buckets();
	}else{
		init_sql();
	}

		start_target_process(argv[1]);
	fprintf(stderr, "[+] CovalyzerRunner initialized. Sending spying pigeons containing test cases...\n");

	//          ╭─────────────────────────────────────────────────────────╮
	//          │                       seed-runs                         │
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
	size_t last_timestamp = 0;

	//Ok, so, normally, in all runs except fuzzbench, we fetch "all" data "at once" (we use step though), and we cancel when sql says enough. But for fuzzbench mode, we must query for each bucket individually, thus, this has to be altered
	//while(sqlite3_step(stmt) == SQLITE_ROW && count <= max_cases){
	while(1){
		int rc = sqlite3_step(stmt);

		if(count == 0 && state_mode == FUZZBENCH){
			first_ts = FIRST_TC_TIMESTAMP;
			set_new_bucket(FIRST_TC_TIMESTAMP);
		}

		//loop cancel conditions
		if(rc != SQLITE_ROW || (max_cases > 0 && count > max_cases)){
			if(state_mode == FUZZBENCH){
				current_bucket++;
				if(max_bucket != -1 && current_bucket > max_bucket){
					fprintf(stderr, "\n" PREF_OK "Reached max. bucket limit. We are done here.\n");
					break;
				}
				update_query_new_bucket();
				backup_cov();
				continue;

			}else{
				break;
			}
		}

		//first, fetch data such that it is available for all thingies
		const void *blob = sqlite3_column_blob(stmt, 0);
		//FIX: use size field
		int blob_size = sqlite3_column_bytes(stmt, 0);
		int current_id = sqlite3_column_int(stmt, 1);
		const char *time_str = (const char *)sqlite3_column_text(stmt, 2);
		time_t timestamp = (time_t)atol(time_str); //(time_t) sqlite3_column_int64(stmt, 2);

		if(count == 0 && state_mode != FUZZBENCH){
			first_ts = timestamp;
			set_new_bucket(timestamp);
			fprintf(stderr, PREF_WARN "First bucket id is: %i\n", current_bucket);
		}
		if(is_new_bucket(timestamp)){
			backup_cov();
			set_new_bucket(timestamp);
		}

		//check if target should be restarted
		if(((last_case == next_restart) && state_mode != FUZZBENCH) || child_exit_flag || state_mode == NON_STATEFUL){
			fprintf(stderr, PREF_WARN "Restart incoming... Last executed id: [%zu]\n", last_case);
			//if(is_new_bucket(timestamp) && state_mode == FUZZBENCH && testset_mode == ALL_CASES){
			//	fprintf(stderr, PREF_WARN "Restart bc. new time bucket\n");
			//	backup_cov();
			//	set_new_bucket(timestamp);
			//	restart_target();
			//}else 
			if(last_case == next_restart){
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

		//check if we have to copy (backup in fb q mode is done in loop cancel)
		//if(state_mode == FUZZBENCH && testset_mode == ALL_CASES){
		//	if(is_new_bucket(timestamp) ){
		//		backup_cov();
		//		if(state_mode != FUZZBENCH){
		//			set_new_bucket(timestamp);
		//		}
		//	}

		//}


		//send testcase, print progress...
		last_case = current_id;
		if(blob && blob_size > 0){
			if(send_testcase(blob, (size_t)blob_size)){
				double progress = (max_cases > 0) ? ((double)total_execs/ max_cases) * 100.0 : 0.0;
				if(state_mode == FUZZBENCH){
					fprintf(stderr, "\r" PREF_OK "Progress: [%.1f%%] | Crashes: [%zu] | Restarts: [%zu] | Timeouts: [%zu] | Bucket: [%i] | total_execs: [%zu] | Executed testcase ID %d (%d bytes)", progress, crashes, restarts, timeouts, current_bucket, total_execs, current_id, blob_size);
					fflush(stderr);	
				}else{
					fprintf(stderr, "\r" PREF_OK "Progress: [%.1f%%] | Crashes: [%zu] | Restarts: [%zu] | Timeouts: [%zu] | total_execs: [%zu] | Executed testcase ID %d (%d bytes)", progress, crashes, restarts, timeouts, total_execs, current_id, blob_size);
				}
				//save exec time 
			}
		}
		count++;
		total_execs++;
	}
	fprintf(stderr, "\n" PREF_OK "Executed all cases, KILLING OUR CHILD...\n");
	run_dead = 1;
	fprintf(stderr, PREF_OK "Done.\n");
	write_exit_log();

	cleanup();
	return 0;
}
