/*
----------- audio-info ---------

This is a program whose main purpose is to calculate
the total duration of all audio files in a directory.
It works on Linux, and may work on other Unixes.
You can run `audio-info` without any arguments to
find the total duration of everything in the current directory
(and all directories in the current directory, etc.).
Check `audio-info --help` for more options.

----------- INSTALLATION -------

Requires libavformat and libavutil. These can be installed on
Debian/Ubuntu with:
sudo apt install libavutil-dev libavformat-dev

Now just run:
sudo make install
and audio-info will be installed to /usr/bin.
You can also just run `make` to compile a binary without installing it.

-----------   LICENSE  ---------
Anyone is free to modify/distribute/use/sell/etc
this software for any purpose by any means.
Any copyright protections are relinquished by the author(s).
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT ANY WARRANTY.
THE AUTHORS SHALL NOT BE HELD LIABLE IN CONNECTION WITH THE SOFTWARE.
*/

#define FILE_LIST_STR_CAP 8000000 // max length of file list string
#define FILE_LIST_INDEX_CAP 2000000
#define PATH_NAME_CAP 100000 // max path len
#define THREADS_MAX 1000

#define _GNU_SOURCE

#include <libavformat/avformat.h>
#include <libavutil/log.h>
#include <dirent.h>
#include <unistd.h>
#include <inttypes.h>
#include <assert.h>
#include <pthread.h>
#include <sys/stat.h>

// sets *len to the duration of the file, in milliseconds
// if accurate is 1, a slower but more accurate method will be used
// returns 0 iff this isn't an audio file
int audio_duration(const char *filename, uint64_t *len, int accurate) {
	int success = 0;
	AVFormatContext *format = NULL;
	if (len) *len = 0;
	if (avformat_open_input(&format, filename, NULL, NULL) == 0) {
		unsigned stream_idx = (unsigned)-1;
		if (avformat_find_stream_info(format, NULL) >= 0) {
			for (unsigned i = 0; i < format->nb_streams; ++i) {
				if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
					stream_idx = i;
				}
			}
		}
		if (stream_idx < format->nb_streams) {
			AVStream *stream = format->streams[stream_idx];
			double time_base = (double)stream->time_base.num / (double)stream->time_base.den;
			double time_in_seconds = 0;
			if (accurate) {
				AVPacket packet = {0};
				while (av_read_frame(format, &packet) >= 0) {
					if (packet.duration > 0xf0000000) continue; // i have a corrupted audio file or something
					time_in_seconds += (double)packet.duration * time_base;
				}
			} else {
				time_in_seconds = (double)stream->duration * time_base;
			}
			if (time_in_seconds >= 0) {
				success = 1;
				if (len) *len = (uint64_t)(time_in_seconds * 1000 + 0.5);
			}
		}
	}
	if (format)
		avformat_close_input(&format);
	return success;
}

typedef struct {
	int32_t str_len;
	int32_t indices_len;
	char str[FILE_LIST_STR_CAP];
	int32_t indices[FILE_LIST_INDEX_CAP];
} FileList;

int file_list_add(FileList *l, const char *filename) {
	size_t flen = strlen(filename);
	if (flen == 0) return 1; // this shouldn't happen
	if ((int)flen + l->str_len + 1 >= FILE_LIST_STR_CAP || l->indices_len + 1 >= FILE_LIST_INDEX_CAP) {
		// end of list (too long/too many indices)
		static int shown = 0;
		if (!shown) {
			fprintf(stderr, "Out of memory. Only partial results will be shown\n");
			shown = 1;
		}
		return 0;
	}
	
	l->indices[l->indices_len++] = l->str_len;
	memcpy(l->str + l->str_len, filename, flen + 1);
	l->str_len += flen + 1;
	return 1;
}

void search_dir(char *path, const char *dir_name, FileList *l, int depth) {
	if (depth <= 0) return;

	size_t plen = strlen(path), dlen = strlen(dir_name);
	if (plen + dlen + 5 > PATH_NAME_CAP)
		return;
	size_t plen_full;
	if (*path == '\0') {
		strcpy(path, dir_name);
		plen_full = dlen;
	} else {
		path[plen] = '/';
		strcpy(&path[plen+1], dir_name);
 		plen_full = plen + dlen + 1;
	}


	DIR *dir = opendir(path);
	if (dir) {
		struct dirent *ent;
		while ((ent = readdir(dir))) {
			switch (ent->d_type) {
			case DT_REG: {
				size_t nlen = strlen(ent->d_name);
				if (nlen + plen_full + 5 > PATH_NAME_CAP)
					break;
				path[plen_full] = '/';
				strcpy(&path[plen_full+1], ent->d_name);
				file_list_add(l, path);
				path[plen_full] = '\0';
			} break;
			case DT_DIR:
				if (ent->d_name[0] != '.') {
					search_dir(path, ent->d_name, l, depth-1);
				}
				break;
			}
		}
		closedir(dir);
	}

	path[plen] = '\0';
}

typedef struct {
	int thread_number;
	const FileList *list;
	int32_t start_pos;
	int32_t count;
	int accurate_duration;
	uint64_t *duration;
	int32_t *file_count;
	uint64_t *size_in_bytes;
} ThreadData;

static pthread_mutex_t g_progress_lock = PTHREAD_MUTEX_INITIALIZER;
static int32_t g_progress = 0;

void *thread_fn(void *vdata) {
	ThreadData *data = (ThreadData *)vdata;
	const FileList *list = data->list;
	int32_t end_pos = data->start_pos + data->count;

	uint64_t duration = 0;
	int32_t file_count = 0;
	int record_total_size = data->size_in_bytes != NULL;
	uint64_t total_size = 0;
	int32_t percent = (data->count + 99) / 100;

	for (int32_t i = data->start_pos; i < end_pos; ++i) {
		const char *filename = &list->str[list->indices[i]];
		uint64_t d = 0;
		if (audio_duration(filename, &d, data->accurate_duration)) {
			++file_count;
			duration += d;

			if (record_total_size) {
				struct stat statbuf = {0};
				stat(filename, &statbuf);
				// NOTE: st_size would give us the file size, not the size on disk
				total_size += (uint64_t)statbuf.st_blocks * 512;
			}
		}

		if (i % percent == 0) {
			pthread_mutex_lock(&g_progress_lock);
			g_progress += percent;
			pthread_mutex_unlock(&g_progress_lock);
		}
	}

	if (data->duration) *data->duration = duration;
	if (data->file_count) *data->file_count = file_count;
	if (data->size_in_bytes) *data->size_in_bytes = total_size;
	
	return NULL;
}

int main(int argc, char **argv) {
	struct timespec start_time = {0}, end_time = {0};
	clock_gettime(CLOCK_MONOTONIC, &start_time);

	static FileList file_list;
	
	int nthreads = (int)sysconf(_SC_NPROCESSORS_ONLN);
	int simple_output = 0;
	int accurate_duration = 0;
	int max_depth = 1000;
	const char *path = NULL;


	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--help") == 0) {
		#ifndef ITALICS
			#define ITALICS(text) "\x1b[3m" text "\x1b[0m"
		#endif
			printf("audio-info - Get information about audio files.\n"
				"Usage:\n"
				"\taudio-info [directory] [options...]\n"
				"\t\tall files in the given directory will be searched (default: .)\n"
				"\taudio-info <list> [options...]\n"
				"\t\twhere list is the name of a file containing a newline-separated list of files to go through\n"
				"Options:\n"
				"\t-a\tUse more accurate but slower method for determining audio duration.\n"
				"\t-d" ITALICS("n") "\tOnly search up to a depth of " ITALICS("n") ".\n"
				"\t-j" ITALICS("n") "\tRun with " ITALICS("n") " threads (defaults to how many threads your CPU has).\n"
				"\t-s\tJust output the number of milliseconds, no additional information (for use in scripts).\n");
			return 0;
		} else if (sscanf(argv[i], "-j%d", &nthreads) == 1) {
		} else if (sscanf(argv[i], "-d%d", &max_depth) == 1) {
		} else if (strcmp(argv[i], "-a") == 0) {
			accurate_duration = 1;
		} else if (strcmp(argv[i], "-s") == 0) {
			simple_output = 1;
		} else if (argv[i][0] != '-' && !path) {
			path = argv[i];
		} else {
			fprintf(stderr, "Unrecognized option: %s\n", argv[i]);
			return -1;
		}
	}

	if (!path) path = ".";

	// get list of files

	struct stat path_stat = {0};
	stat(path, &path_stat);
	if (S_ISREG(path_stat.st_mode)) {
		size_t file_size = (size_t)path_stat.st_size;
		if (file_size >= FILE_LIST_STR_CAP) {
			fprintf(stderr, "Too many files specified.\n");
			return -1;
		}
		
		FILE *fp = fopen(path, "r");
		if (fp) {
			char *str = file_list.str;
			fread(str, 1, file_size, fp);
			file_list.indices[0] = 0;
			file_list.indices_len = 1;
			for (int32_t i = 0; i < (int32_t)file_size; ++i) {
				if (str[i] == '\n') {
					str[i] = '\0';
					if (file_list.indices_len >= FILE_LIST_INDEX_CAP) {
						fprintf(stderr, "Too many files specified.\n");
						return -1;
					}
					file_list.indices[file_list.indices_len++] = i+1;
				}
			}
			str[file_size] = '\0';
		} else {
			perror("Couldn't open list");
		}
	} else if (S_ISDIR(path_stat.st_mode)) {
		static char dir_name[PATH_NAME_CAP];
		search_dir(dir_name, path, &file_list, max_depth);
	} else {
		printf("Argument '%s' is not a file or a directory.\n", path);
		return -1;
	}

	av_log_set_level(AV_LOG_QUIET);

	if (nthreads <= 0 || nthreads > THREADS_MAX) nthreads = 8;


	{
		static uint64_t durations[THREADS_MAX], sizes_in_bytes[THREADS_MAX];
		static int32_t file_counts[THREADS_MAX];
		static pthread_t threads[THREADS_MAX];
		static int active[THREADS_MAX];
		static ThreadData thread_datas[THREADS_MAX];

		int32_t batch_size = (file_list.indices_len + nthreads - 1) / nthreads;
		int32_t remaining = file_list.indices_len;

		for (int i = 0; i < nthreads; ++i) {
			int32_t count = batch_size;
			if (i == nthreads - 1) count = remaining;
			if (count > remaining) count = remaining;
			if (count > 0) {
				ThreadData* data = &thread_datas[i];
				data->list = &file_list;
				data->start_pos = file_list.indices_len - remaining;
				data->count = count;
				data->duration = &durations[i];
				if (!simple_output) {
					data->file_count = &file_counts[i];
					data->size_in_bytes = &sizes_in_bytes[i];
				}
				data->thread_number = i;
				data->accurate_duration = accurate_duration;

				if (pthread_create(&threads[i], NULL, thread_fn, data) == 0) {
					active[i] = 1;
				} else {
					fprintf(stderr, "Error starting thread #%d.\n", i);
					exit(-1);
				}
				remaining -= count;
			}
		}
		assert(remaining == 0);

		if (!simple_output) {
			int n_active = 0;
			for (int i = 0; i < nthreads; ++i) n_active += active[i];
			printf("Running on %d thread%s.\n", n_active, n_active == 1 ? "" : "s");
			fflush(stdout);
		}

		if (simple_output) {
			// don't print progress reports
			for (int i = 0; i < nthreads; ++i) {
				if (active[i])
					pthread_join(threads[i], NULL);
			}
		} else {
			while (1) {
				int any_still_running = 0;
				usleep(16000);
				
				for (int i = 0; i < nthreads; ++i) {
					if (active[i]) {
						if (pthread_tryjoin_np(threads[i], NULL) == 0) {
							active[i] = 0;
						} else {
							any_still_running = 1;
						}
					}
				}

				if (!any_still_running)
					break;
				
				pthread_mutex_lock(&g_progress_lock);
				int32_t progress = g_progress;
				pthread_mutex_unlock(&g_progress_lock);

				printf("\r%" PRId32 "/%" PRId32 " files processed...        ", progress, file_list.indices_len);
				fflush(stdout);
			}
		}
		printf("\r                                                  \r");
		fflush(stdout);

		{
			uint64_t total_duration = 0;
			int32_t total_file_count = 0;
			uint64_t total_size = 0;

			if (!simple_output) printf("Done.\n");

			for (int i = 0; i < nthreads; ++i) {
				total_duration += durations[i];
				total_file_count += file_counts[i];
				total_size += sizes_in_bytes[i];
			}

			if (simple_output) {
				printf("%" PRIu64 "\n", total_duration);
				return 0;
			}

			clock_gettime(CLOCK_MONOTONIC, &end_time);
			printf("----- STATISTICS -----\n");

			{
				double dt = (double)(end_time.tv_sec - start_time.tv_sec) + 1e-9 * (double)(end_time.tv_nsec - start_time.tv_nsec);

				if (dt > 120) {
					printf("Processing time: %ld minutes\n", (long)(dt / 60));
				} else {
					printf("Processing time: %.02f seconds\n", dt);
				}
			}

			{
				uint64_t d, hours, minutes, seconds, milliseconds;
				d = total_duration;
				hours =  d / 3600000;
				d -= hours * 3600000;
				minutes  = d / 60000;
				d -= minutes * 60000;
				seconds  = d / 1000;
				d -= seconds * 1000;
				milliseconds = d;
				printf("%" PRId32 " audio files\n", total_file_count);
				printf("Total duration: %" PRIu64 ":%02" PRIu64 ":%02" PRIu64 ".%03" PRIu64 " ", hours, minutes, seconds, milliseconds);
				printf("(%" PRIu64 " seconds)\n", total_duration / 1000);
			}

			{
				double size = (double)total_size;
				const char *units = "B";
				if (size > 1000) {
					size /= 1024;
					units = "KB";
				}
				if (size > 1000) {
					size /= 1024;
					units = "MB";
				}
				if (size > 1000) {
					size /= 1024;
					units = "GB";
				}
				if (size > 1000) {
					size /= 1024;
					units = "TB";
				}
				printf("Total size on disk: %.2f%s\n", size, units);
			}
		}

	}
	
	return 0;
}
