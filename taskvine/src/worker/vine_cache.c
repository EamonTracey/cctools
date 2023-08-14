/*
Copyright (C) 2022- The University of Notre Dame
This software is distributed under the GNU General Public License.
See the file COPYING for details.
*/

#include "vine_worker.h"
#include "vine_cache.h"
#include "vine_process.h"
#include "vine_mount.h"
#include "vine_sandbox.h"

#include "vine_transfer.h"
#include "vine_protocol.h"

#include "xxmalloc.h"
#include "hash_table.h"
#include "debug.h"
#include "stringtools.h"
#include "trash.h"
#include "link.h"
#include "link_auth.h"
#include "timestamp.h"
#include "copy_stream.h"
#include "path_disk_size_info.h"

#include <sys/types.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

struct vine_cache {
	struct hash_table *table;
	char *cache_dir;
};

struct cache_file {
        vine_cache_type_t type;
        timestamp_t start_time;
        timestamp_t stop_time;
        pid_t pid;
        char *source;
        int64_t actual_size;
        int mode;
        int status;
        struct vine_task *mini_task;
	struct vine_process *process;
};

struct cache_file * cache_file_create( vine_cache_type_t type, const char *source, int64_t actual_size, int mode, struct vine_task *mini_task )
{
	struct cache_file *f = malloc(sizeof(*f));
	f->type = type;
	f->source = xxstrdup(source);
	f->actual_size = actual_size;
	f->mode = mode;
	f->pid = 0;
	f->status = VINE_CACHE_STATUS_NOT_PRESENT;
	f->mini_task = mini_task;
	f->process = 0;
	f->start_time = 0;
	f->stop_time = 0;
	return f;
}

void cache_file_delete( struct cache_file *f )
{
	if(f->mini_task) {
		vine_task_delete(f->mini_task);
	}

	free(f->source);
	free(f);
}

/*
Create the cache manager structure for a given cache directory.
*/

struct vine_cache * vine_cache_create( const char *cache_dir )
{
	struct vine_cache *c = malloc(sizeof(*c));
	c->cache_dir = strdup(cache_dir);
	c->table = hash_table_create(0,0);
	return c;
}

/*
Load existing cache directory into cache structure.
*/
void vine_cache_load(struct vine_cache *c)
{
	DIR *dir = opendir(c->cache_dir);
	if(dir){
		debug(D_VINE, "loading cache at: %s", c->cache_dir);
		struct dirent *d;
		while((d=readdir(dir))){
			if(!strcmp(d->d_name,".")) continue;
			if(!strcmp(d->d_name,"..")) continue;

			debug(D_VINE, "found %s in cache",d->d_name);

			struct stat info;
			int64_t nbytes, nfiles;
			char *cache_path = vine_cache_full_path(c,d->d_name);

			if(stat(cache_path, &info)==0){
				if(S_ISREG(info.st_mode)){
					vine_cache_addfile(c, info.st_size, info.st_mode, d->d_name);
				} else if(S_ISDIR(info.st_mode)){
                			path_disk_size_info_get(cache_path,&nbytes,&nfiles); 
					vine_cache_addfile(c, nbytes, info.st_mode, d->d_name);
				}
			} else {
				debug(D_VINE,"could not stat: %s in cache: %s error %s", d->d_name, c->cache_dir, strerror(errno));
			}
			free(cache_path);	
		}
	}
	closedir(dir);
}

/*
send cache updates to manager from existing cache_directory 
*/

void vine_cache_scan(struct vine_cache *c, struct link *manager)
{
	struct cache_file *f;
	char * cachename;
	HASH_TABLE_ITERATE(c->table, cachename, f){
		/* XXX the worker doesn't know how long it took to transfer. */
		vine_worker_send_cache_update(manager,cachename,f->actual_size,0,0);
	}
}

/*
Delete the cache manager structure, though not the underlying files.
*/
void vine_cache_delete( struct vine_cache *c )
{
	hash_table_clear(c->table,(void*)cache_file_delete);
	hash_table_delete(c->table);
	free(c->cache_dir);
	free(c);
}

/*
Get the full path to a file name within the cache.
This result must be freed.
*/

char * vine_cache_full_path( struct vine_cache *c, const char *cachename )
{
	return string_format("%s/%s",c->cache_dir,cachename);
}
	

/*
Add a file to the cache manager (already created in the proper place) and note its size.
*/

int vine_cache_addfile( struct vine_cache *c, int64_t size, int mode, const char *cachename )
{
	struct cache_file *f = hash_table_lookup(c->table,cachename);
	if(!f) {
		f = cache_file_create(VINE_CACHE_FILE,"manager",size,mode,0);
		hash_table_insert(c->table,cachename,f);
	}

	f->status = VINE_CACHE_STATUS_READY;
	return 1;
}

/*
Return true if the cache contains the requested item.
*/

int vine_cache_contains( struct vine_cache *c, const char *cachename )
{
	return hash_table_lookup(c->table,cachename)!=0;
}

/*
Queue a remote file transfer to produce a file.
This entry will be materialized later in vine_cache_ensure.
*/

int vine_cache_queue_transfer( struct vine_cache *c, const char *source, const char *cachename, int64_t size, int mode )
{
	struct cache_file *f = cache_file_create(VINE_CACHE_TRANSFER,source,size,mode,0);
	hash_table_insert(c->table,cachename,f);
	return 1;
}

/*
Queue a mini-task to produce a file.
This entry will be materialized later in vine_cache_ensure.
*/

int vine_cache_queue_command( struct vine_cache *c, struct vine_task *mini_task, const char *cachename, int64_t size, int mode )
{
	struct cache_file *f = cache_file_create(VINE_CACHE_MINI_TASK,"task",size,mode,mini_task);
	hash_table_insert(c->table,cachename,f);
	return 1;
}

/*
Remove a named item from the cache, regardless of its type.
*/

int vine_cache_remove( struct vine_cache *c, const char *cachename )
{
	struct cache_file *f = hash_table_remove(c->table,cachename);
	if(!f) return 0;

	char *cache_path = vine_cache_full_path(c,cachename);
	trash_file(cache_path);
	free(cache_path);

	cache_file_delete(f);

	return 1;

}

/*
Execute a shell command via popen and capture its output.
On success, return true.
On failure, return false with the string error_message filled in.
*/


static int do_internal_command( struct vine_cache *c, const char *command, char **error_message )
{
	int result = 0;
	*error_message = 0;
	
	debug(D_VINE,"executing: %s",command);
		
	FILE *stream = popen(command,"r");
	if(stream) {
		copy_stream_to_buffer(stream,error_message,0);
	  	int exit_status = pclose(stream);
		if(exit_status==0) {
			if(*error_message) {
				free(*error_message);
				*error_message = 0;
			}
			result = 1;
		} else {
			debug(D_VINE,"command failed with output: %s",*error_message);
			result = 0;
		}
	} else {
		*error_message = string_format("couldn't execute \"%s\": %s",command,strerror(errno));
		result = 0;
	}

	return result;
}

/*
Transfer a single input file from a url to a local filename by using /usr/bin/curl.
-s Do not show progress bar.  (Also disables errors.)
-S Show errors.
-L Follow redirects as needed.
--stderr Send errors to /dev/stdout so that they are observed by popen.
*/

static int do_curl_transfer( struct vine_cache *c, const char *source_url, const char *cache_path, char **error_message )
{
	char * command = string_format("curl -sSL --stderr /dev/stdout -o \"%s\" \"%s\"",cache_path,source_url);
	int result = do_internal_command(c,command,error_message);
	free(command);
	return result;
}

/*
Create a file by executing a mini_task, which should produce the desired cachename.
The mini_task uses all the normal machinery to run a task synchronously,
which should result in the desired file being placed into the cache.
This will be double-checked below.
*/

static int do_mini_task( struct vine_cache *c, struct cache_file *f, char **error_message )
{
	if(vine_process_execute_and_wait(f->process,c)) {
		*error_message = 0;
		return 1;
	} else {
		const char *str = vine_task_get_stdout(f->mini_task);
		if(str) {
			*error_message = xxstrdup(str);
		} else {
			*error_message = 0;
		}
		return 0;
	}
}

/*
Transfer a single input file from a worker url to a local file name. 

*/
static int do_worker_transfer( struct vine_cache *c, const char *source_url, const char *cache_path, char **error_message)
{	
	int port_num;
	char addr[VINE_LINE_MAX], path[VINE_LINE_MAX];
	int stoptime;	
	struct link *worker_link;
	
	// expect the form: worker://addr:port/path/to/file
	sscanf(source_url, "worker://%99[^:]:%d/%s", addr, &port_num, path);
	debug(D_VINE, "Setting up worker transfer file %s",source_url);

	stoptime = time(0) + 15;
	worker_link = link_connect(addr, port_num, stoptime);

	if(worker_link==NULL) {
		*error_message = string_format("Could not establish connection with worker at: %s:%d", addr, port_num);
		return 0;
	}

	if(vine_worker_password) {
		if(!link_auth_password(worker_link,vine_worker_password,time(0)+5)) {
			*error_message = string_format("Could not authenticate to peer worker at %s:%d", addr, port_num);
			link_close(worker_link);
			return 0;
		}
	}
	
	/* XXX A fixed timeout of 900 certainly can't be right! */
	
	if(!vine_transfer_get_any(worker_link, c, path, time(0) + 900))
	{
		*error_message = string_format("Could not transfer file %s from worker %s:%d", path, addr, port_num);
		link_close(worker_link);
		return 0;
	}
		
	
	link_close(worker_link);

	return 1;
}

/*
Transfer a single obejct into the cache,
whether by worker or via curl.
Use a temporary transfer path while downloading,
and then rename it into the proper place.
*/

static int do_transfer( struct vine_cache *c, const char *source_url, const char *cache_path, char **error_message)
{
	char *transfer_path = string_format("%s.transfer",cache_path);
	int result = 0;
	
	if(strncmp(source_url, "worker://", 9) == 0){
		result = do_worker_transfer(c,source_url,transfer_path,error_message);
		if(result){
			debug(D_VINE, "received file from worker");
			rename(cache_path, transfer_path);
		}
	} else { 
		result = do_curl_transfer(c,source_url,transfer_path,error_message);
	}

	if(result) {
		if(rename(transfer_path,cache_path)==0) {
			debug(D_VINE,"cache: renamed %s to %s",transfer_path,cache_path);
		} else {
			debug(D_VINE,"cache: failed to rename %s to %s: %s",transfer_path,cache_path,strerror(errno));
			result = 0;
		}
	}

	if(!result) trash_file(transfer_path);
	
	free(transfer_path);

	return result;
}

static void vine_cache_handle_exit_status(int status, char *cachename, struct cache_file *f, struct link *manager){	
	int exit_code;
	f->stop_time = timestamp_get();
	if(!WIFEXITED(status)){
		exit_code = WTERMSIG(status);
		debug(D_VINE, "transfer process (pid %d) exited abnormally with signal %d",f->pid, exit_code);
		f->status = VINE_CACHE_STATUS_FAILED;
	} else {
		exit_code = WEXITSTATUS(status);
		debug(D_VINE, "transfer process for %s (pid %d) exited normally with exit code %d", cachename, f->pid, exit_code );
		if(exit_code==1){	
			debug(D_VINE, "transfer process for %s completed", cachename);
			f->status = VINE_CACHE_STATUS_READY;
		} else {
			debug(D_VINE, "transfer process for %s failed", cachename);
			f->status = VINE_CACHE_STATUS_FAILED;
		}
	}
}

static void vine_cache_check_outputs(struct cache_file *f, char *cachename, struct vine_cache *c, struct link *manager)
{
	int64_t nbytes, nfiles;
	char *cache_path = vine_cache_full_path(c,cachename);
	timestamp_t transfer_time = f->stop_time - f->start_time;
	if(f->type==VINE_CACHE_MINI_TASK){
		// stageout only if process succeeded 
		if(f->status==VINE_CACHE_STATUS_READY) vine_sandbox_stageout(f->process, c, manager);
		f->process->task = 0;
		vine_process_delete(f->process);
	}
	if(f->status==VINE_CACHE_STATUS_READY){
		chmod(cache_path,f->mode);
		if(path_disk_size_info_get(cache_path,&nbytes,&nfiles)==0) {
			f->actual_size = nbytes;
			debug(D_VINE,"cache: created %s with size %lld in %lld usec",cachename,(long long)f->actual_size,(long long)transfer_time);
			vine_worker_send_cache_update(manager,cachename,f->actual_size,transfer_time,f->start_time);
			f->status = VINE_CACHE_STATUS_READY;
		} else {
			debug(D_VINE,"cache: command succeeded but did not create %s",cachename);
			f->status = VINE_CACHE_STATUS_FAILED;
		}

	} else {
		debug(D_VINE,"cache: unable to create %s",cachename);
	}
	free(cache_path);
}

static void vine_cache_process_entry(struct cache_file *f, char *cachename, struct vine_cache *c, struct link *manager)
{
	int status;
	if(f->status==VINE_CACHE_STATUS_PROCESSING){
		int result = waitpid(f->pid, &status, WNOHANG);
		if(result==0){
			// process stil executing
		} else if(result<0) {
			debug(D_VINE, "wait4 on pid %d returned an error: %s",(int)f->pid,strerror(errno));	
		} else if(result>0) {
			vine_cache_handle_exit_status(status,cachename,f,manager);
			vine_cache_check_outputs(f,cachename,c,manager);
		}
	}
}

int vine_cache_wait(struct vine_cache *c, struct link *manager)
{
	struct cache_file *f;
    	char *cachename;
	HASH_TABLE_ITERATE(c->table, cachename, f){
		vine_cache_process_entry(f,cachename,c,manager);
	}
	return 1;

}

/*
Ensure that a given cached entry is fully materialized in the cache,
downloading files or executing commands as needed.  If complete, return
VINE_CACHE_STATUS_READY, If downloading return VINE_CACHE_STATUS_PROCESSING.
On failure return VINE_CACHE_STATUS_FAILED.

*/

vine_cache_status_type_t vine_cache_ensure( struct vine_cache *c, const char *cachename)
{
	if(!strcmp(cachename,"0")) return VINE_CACHE_STATUS_READY;

	struct cache_file *f = hash_table_lookup(c->table,cachename);
	if(!f) {
		debug(D_VINE,"cache: %s is unknown, perhaps it failed to transfer earlier?",cachename);
		return VINE_CACHE_STATUS_FAILED;
	}

	/* File is already present in the cache. */
	switch(f->status) {
		case VINE_CACHE_STATUS_READY:
			return VINE_CACHE_STATUS_READY;
		case VINE_CACHE_STATUS_FAILED:
			return VINE_CACHE_STATUS_FAILED;
		case VINE_CACHE_STATUS_PROCESSING:
			return VINE_CACHE_STATUS_PROCESSING;
		case VINE_CACHE_STATUS_NOT_PRESENT:
			break;

	}

	if(f->type == VINE_CACHE_MINI_TASK){
		if(f->mini_task->input_mounts) {
                	struct vine_mount *m;
			vine_cache_status_type_t result;
                	LIST_ITERATE(f->mini_task->input_mounts,m) {
                        	result = vine_cache_ensure(c,m->file->cached_name);
                        	if(result!=VINE_CACHE_STATUS_READY) return result;
                	}
        	}
	}
	
	f->start_time = timestamp_get();

	debug(D_VINE,"forking transfer process to create %s", cachename);

	struct vine_process *p;
	if(f->type == VINE_CACHE_MINI_TASK){
		p = vine_process_create(f->mini_task, 1);
		if(!vine_sandbox_stagein(p,c)) {
			debug(D_VINE, "Can't stage input files for task %d.", p->task->task_id);
			p->task = 0;
			vine_process_delete(p);
			f->status = VINE_CACHE_STATUS_FAILED;
			return VINE_CACHE_STATUS_FAILED;
		}
		f->process = p;
	}

	pid_t pid = fork();

	if(pid == -1) {
		debug(D_VINE,"failed to fork transfer process");
		return VINE_CACHE_STATUS_FAILED;
	}
	if(pid > 0){
		f->pid = pid;
		f->status = VINE_CACHE_STATUS_PROCESSING;
		switch(f->type){
			case (VINE_CACHE_TRANSFER):
				debug(D_VINE,"cache: transferring %s to %s",f->source,cachename);
				break;
			case (VINE_CACHE_MINI_TASK): 
				debug(D_VINE,"cache: creating %s via mini task",cachename);
				break;
			case (VINE_CACHE_FILE):
				debug(D_VINE,"cache: checking if %s is present in cache",cachename);
				break;
		}
		return VINE_CACHE_STATUS_PROCESSING;
	}

	vine_cache_get_file(f, c, cachename);
	exit(1);

}
/*
Child Process that materializes the proper file.

*/

void vine_cache_get_file(struct cache_file *f, struct vine_cache *c, const char *cachename){

	char *error_message = 0;
	char *cache_path = vine_cache_full_path(c,cachename);
	int result = 0;

	switch(f->type) {
		case VINE_CACHE_FILE:
			result = 1;
			break;
		case VINE_CACHE_TRANSFER:
			result = do_transfer(c,f->source,cache_path,&error_message);
			break;

		case VINE_CACHE_MINI_TASK:
			result = do_mini_task(c,f,&error_message);
			break;
	}
	if(error_message){ 
		debug(D_VINE,"An error occured when creating %s via mini task: %s", cachename, error_message);
		free(error_message);
	}
	free(cache_path);
	exit(result);
}

