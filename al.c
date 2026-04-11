#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <time.h>
#include <math.h>
#include <stdbool.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>

#define INPUT_BY_PATH_DIR "/dev/input/by-path"

typedef struct {
	int fd;
	char *path;
	bool isMouse;
} InputDevice;

typedef struct {
	pthread_mutex_t mutex;
	double windowStartTime;
	double nextWindowTime;
	double windowDuration;
	unsigned int windowClicks;
	bool windowMouseActivity;
	double windowFirstClickTime;
	double windowLastClickTime;
	double windowFirstMouseTime;
	double windowLastMouseTime;
} ActivityState;

typedef struct {
	const InputDevice *device;
	ActivityState *state;
} DeviceThreadCtx;

static atomic_bool g_running = true;

static void onSignal(int signum) {
	(void)signum;
	atomic_store(&g_running, false);
}

static void freeDevices(InputDevice *devices, size_t count) {
	if (!devices) return;
	for (size_t i = 0; i < count; i++) {
		if (devices[i].fd >= 0) close(devices[i].fd);
		free(devices[i].path);
	}
	free(devices);
}

static int openInputDevicesByPath(InputDevice **outDevices, size_t *outCount) {
	if (!outDevices || !outCount) return -1;
	*outDevices = NULL;
	*outCount = 0;

	DIR *dir = opendir(INPUT_BY_PATH_DIR);
	if (!dir) {
		fprintf(stderr, "Failed to open %s: %s\n", INPUT_BY_PATH_DIR, strerror(errno));
		return -1;
	}

	size_t cap = 16;
	InputDevice *devices = calloc(cap, sizeof(*devices));
	if (!devices) {
		closedir(dir);
		fprintf(stderr, "Out of memory\n");
		return -1;
	}
	for (size_t i = 0; i < cap; i++) devices[i].fd = -1;
	struct dirent *ent;
	while ((ent = readdir(dir)) != NULL) {
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
		// Most useful nodes are symlinks ending in "-event-*" (e.g. platform-...-event-kbd)
		if (!strstr(ent->d_name, "event")) continue;

		size_t fullLen = strlen(INPUT_BY_PATH_DIR) + 1 + strlen(ent->d_name) + 1;
		char *fullPath = malloc(fullLen);
		if (!fullPath) {
			fprintf(stderr, "Out of memory\n");
			continue;
		}
		int n = snprintf(fullPath, fullLen, "%s/%s", INPUT_BY_PATH_DIR, ent->d_name);
		if (n < 0 || (size_t) n >= fullLen) {
			free(fullPath);
			continue;
		}

		struct stat st;
		if (lstat(fullPath, &st) != 0) {
			fprintf(stderr, "Skipping %s: %s\n", fullPath, strerror(errno));
			free(fullPath);
			continue;
		}
		if (!S_ISLNK(st.st_mode) && !S_ISCHR(st.st_mode) && !S_ISREG(st.st_mode)) {
			free(fullPath);
			continue;
		}

		int fd = open(fullPath, O_RDONLY);
		if (fd < 0) {
			fprintf(stderr, "Failed to open %s: %s\n", fullPath, strerror(errno));
			free(fullPath);
			continue;
		}

		if (*outCount == cap) {
			size_t newCap = cap * 2;
			InputDevice *newDevices = realloc(devices, newCap * sizeof(*newDevices));
			if (!newDevices) {
				close(fd);
				free(fullPath);
				freeDevices(devices, *outCount);
				closedir(dir);
				fprintf(stderr, "Out of memory\n");
				return -1;
			}
			devices = newDevices;
			for (size_t i = cap; i < newCap; i++) {
				devices[i].fd = -1;
				devices[i].path = NULL;
			}
			cap = newCap;
		}

		devices[*outCount].fd = fd;
		devices[*outCount].path = fullPath;
		devices[*outCount].isMouse = (strstr(fullPath, "mouse") != NULL);
		(*outCount)++;
	}

	closedir(dir);
	if (*outCount == 0) {
		free(devices);
		fprintf(stderr, "No input devices opened from %s\n", INPUT_BY_PATH_DIR);
		return -1;
	}

	*outDevices = devices;
	return 0;
}

double getCurrentTime() {
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		return (double) ts.tv_sec + ts.tv_nsec / 1e9;
}

static void *watchDeviceThread(void *arg) {
	DeviceThreadCtx *ctx = (DeviceThreadCtx *)arg;
	struct input_event ev;
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

	while (atomic_load(&g_running)) {
		ssize_t r = read(ctx->device->fd, &ev, sizeof(ev));
		if (r == (ssize_t) sizeof(ev)) {
			pthread_mutex_lock(&ctx->state->mutex);
			if (ctx->device->isMouse) {
				ctx->state->windowMouseActivity = true;
				ctx->state->windowLastMouseTime = getCurrentTime();
				if (ctx->state->windowFirstMouseTime == 0) {
					ctx->state->windowFirstMouseTime = ctx->state->windowLastMouseTime;
				}
			} else {
				if (ev.type == EV_KEY && ev.value == 1) {
					ctx->state->windowClicks++;
					ctx->state->windowLastClickTime = getCurrentTime();
					if (ctx->state->windowFirstClickTime == 0) {
						ctx->state->windowFirstClickTime = ctx->state->windowLastClickTime;
					}
				}
			}
			pthread_mutex_unlock(&ctx->state->mutex);
			continue;
		}
		if (r < 0 && errno == EINTR) continue;
		// Device closed/removed or error
		break;
	}

	return NULL;
}

int main(int argc, char *argv[]) {
	if (argc < 1 || argc > 1) {
		fprintf(stderr, "Usage: [sudo] %s\n", argv[0]);
		return 1;
	}
	// Ensure running as root or with appropriate permissions
	InputDevice *devices = NULL;
	size_t deviceCount = 0;
	if (openInputDevicesByPath(&devices, &deviceCount) != 0) {
		fprintf(stderr, "Tip: try running with sudo or add yourself to the input group.\n");
		return 1;
	}

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = onSignal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	ActivityState state;
	pthread_mutex_init(&state.mutex, NULL);
	state.windowDuration = 15.0;
	double now = getCurrentTime();
	state.windowStartTime = floor(now);
	state.nextWindowTime = state.windowStartTime + state.windowDuration;
	state.windowClicks = 0;
	state.windowFirstClickTime = 0;
	state.windowLastClickTime = 0;
	state.windowFirstMouseTime = 0;
	state.windowLastMouseTime = 0;
	state.windowMouseActivity = false;
	pthread_t *threads = calloc(deviceCount, sizeof(*threads));
	DeviceThreadCtx *ctxs = calloc(deviceCount, sizeof(*ctxs));
	if (!threads || !ctxs) {
		fprintf(stderr, "Out of memory\n");
		free(threads);
		free(ctxs);
		pthread_mutex_destroy(&state.mutex);
		freeDevices(devices, deviceCount);
		return 1;
	}

	for (size_t i = 0; i < deviceCount; i++) {
		ctxs[i].device = &devices[i];
		ctxs[i].state = &state;
		int rc = pthread_create(&threads[i], NULL, watchDeviceThread, &ctxs[i]);
		if (rc != 0) {
			fprintf(stderr, "Failed to create thread for %s: %s\n", devices[i].path, strerror(rc));
			atomic_store(&g_running, false);
			deviceCount = i;
			break;
		}
	}
	fprintf(stderr, "Watching %zu input devices for activity (Ctrl-C to stop)...\n", deviceCount);
	while (atomic_load(&g_running)) {
		double nextTime;
		pthread_mutex_lock(&state.mutex);
		nextTime = state.nextWindowTime;
		pthread_mutex_unlock(&state.mutex);

		now = getCurrentTime();
		double sleepSec = nextTime - now;
		if (sleepSec > 0) {
			struct timespec req;
			req.tv_sec = (time_t)sleepSec;
			req.tv_nsec = (long)((sleepSec - (double)req.tv_sec) * 1e9);
			while (atomic_load(&g_running) && nanosleep(&req, &req) != 0 && errno == EINTR) {
				// retry
			}
		}

		now = getCurrentTime();
		pthread_mutex_lock(&state.mutex);
		if (now >= state.nextWindowTime) {
			printf("%llu,%u,%u,%lf,%lf,%lf,%lf\n",
					(unsigned long long)state.windowStartTime, state.windowClicks, state.windowMouseActivity ? 1 : 0,
					state.windowFirstClickTime, state.windowLastClickTime,
					 state.windowFirstMouseTime, state.windowLastMouseTime);
			fflush(stdout);
			state.windowStartTime = floor(now);
			state.nextWindowTime = state.windowStartTime + state.windowDuration;
			state.windowClicks = 0;
			state.windowFirstClickTime = 0;
			state.windowLastClickTime = 0;
			state.windowFirstMouseTime = 0;
			state.windowLastMouseTime = 0;
			state.windowMouseActivity = false;
		}
		pthread_mutex_unlock(&state.mutex);
	}
	for (size_t i = 0; i < deviceCount; i++) {
		pthread_cancel(threads[i]);
	}
	for (size_t i = 0; i < deviceCount; i++) {
		pthread_join(threads[i], NULL);
	}
	free(threads);
	free(ctxs);
	pthread_mutex_destroy(&state.mutex);
	freeDevices(devices, deviceCount);
	return 0;
}

