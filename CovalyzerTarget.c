/*===- StandaloneFuzzTargetMain.c - standalone main() for fuzz targets. ---===//
									      //
									      // Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
									      // See https://llvm.org/LICENSE.txt for license information.
									      // SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
									      //
									      //===----------------------------------------------------------------------===//
									      // This main() function can be linked to a fuzz target (i.e. a library
									      // that exports LLVMFuzzerTestOneInput() and possibly LLVMFuzzerInitialize())
									      // instead of libFuzzer. This main() function will not perform any fuzzing
									      // but will simply feed all input files one by one to the fuzz target.
									      //
									      // Use this file to provide reproducers for bugs when linking against libFuzzer
									      // or other fuzzing engine is undesirable.
									      //===----------------------------------------------------------------------===*/
#include <assert.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h> 
#include <stdbool.h> 


#define SHM_NAME "/covalyzer_shm"
#define SHM_SIZE (1024 * 1024 * 100) // 1 MB 
#define MAX_INPUT_SIZE (SHM_SIZE - sizeof(struct shm_state))

struct shm_state {
	volatile int ready; // set to one by host when data is ready 
	volatile int processed; // set to 1 by target when processing is done 
	volatile atomic_int target_ready; 
	uint8_t modeset;			//
	size_t size; 		// input size 
	uint8_t data[]; 	// flexible array of input data 
};

extern int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size);
__attribute__((weak)) extern int LLVMFuzzerInitialize(int *argc, char ***argv);
extern int __llvm_profile_write_file(void);

static volatile int running = 1;
struct shm_state *state = NULL;
char *instance_id;
char path[256];

int log_fd;
FILE *log_ptr;


void silence_stdout(){
	log_fd = dup(STDERR_FILENO);
	int null_fd = open("/dev/null", O_RDWR);
	dup2(null_fd, STDOUT_FILENO);
	dup2(null_fd, STDERR_FILENO);
	close(null_fd);

	//silence 
	
	dprintf(log_fd, "foo\n");
	log_ptr = fdopen(log_fd, "w");
}

void handle_sigterm(int sig){
	(void)sig;
	running = 0;
	fprintf(stderr, "\n[Target] Got sigterm, setting running to 0\n");
	__llvm_profile_write_file();
	fprintf(stderr, "\n[Target] Cov dumped, exiting...\n");
	_exit(128+sig);
	return;
	if(state->modeset){
		//modeset &=1 = 1 -> INSANE
		fprintf(stderr, "\n[!] [TARGET] Crash detected (signal %d), NOT dumping coverage...\n", sig);
	}else{
		fprintf(stderr, "\n[!] [TARGET] Crash detected (signal %d). Dumping coverage...\n(Well, we hope it __llvm_profile_write_files)\n", sig);
		__llvm_profile_write_file();
		fprintf(stderr, "Cov written!");

	}
	
	_exit(128+sig);

}

//crash handler for "sane" mode
void handle_crash(int sig){
	if(state->modeset){
		//modeset &=1 = 1 -> INSANE
		fprintf(stderr, "\n[!] [TARGET] Crash detected (signal %d), NOT dumping coverage...\n", sig);
	}else{
		fprintf(stderr, "\n[!] [TARGET] Crash detected (signal %d). Dumping coverage...\n(Well, we hope it __llvm_profile_write_files)\n", sig);
		__llvm_profile_write_file();
		fprintf(stderr, "Cov written!");

	}
	
	_exit(128+sig);
}

void install_crash_handlers(){
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handle_crash;

	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGABRT, &sa, NULL);
	sigaction(SIGFPE, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);

	struct sigaction sa_;
	memset(&sa_, 0, sizeof(sa_));
	sa_.sa_handler = handle_sigterm;

	sigaction(SIGTERM, &sa_, NULL);
	sigaction(SIGINT, &sa_, NULL);

}

int main(int argc, char **argv) {
	setbuf(stderr, NULL);
	//silence_stdout();
	setenv("HB_DEBUG", "0", 1);
	fprintf(stderr, "StandaloneFuzzTargetMain: running %d inputs\n", argc - 1);
	if (LLVMFuzzerInitialize)
		LLVMFuzzerInitialize(&argc, &argv);

	instance_id = getenv("CCOV_INSTANCE");
	if(instance_id == NULL){
		fprintf(stderr, "[TARGET] ERROR: NO INSTANCE ID! QUITTING!");
	}
	fprintf(stderr, "[Target] instance id is: %s", instance_id);

	strcpy(path, "/ccov-");
	strcat(path, instance_id);
	int fd = shm_open(path, O_RDWR, 0600);
	if(fd < 0) {
		perror("shm_open");
		exit(1);
	}


	void *shm = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 );
	if(shm == MAP_FAILED){
		perror("mmap");
		exit(1);
	}

	state = (struct shm_state *)shm;
	install_crash_handlers();

	state->ready=1;
	state->target_ready = 1;
	fprintf(stderr, "[CovalyzerTarget] Waiting for input...\n");

	while(running){
		while(!state->ready && running){
			usleep(100); // wait until input is ready 
		}

		if (!running) break;

		if(state->size > 0 && state->size <= MAX_INPUT_SIZE){
			LLVMFuzzerTestOneInput(state->data, state->size);
		}

		state->ready = 0;
		state->processed = 1;
		//we are done here
		if(state->modeset & 2){
			running = 0;
		}
	}

	fprintf(stderr, "[CovalyzerTarget] Shutting down\n");
	fprintf(stderr, "[Target] Dumping coverage on exit...\n");
	__llvm_profile_write_file();
	//munmap(shm, SHM_SIZE);
	close(fd);
	return 0;
}
