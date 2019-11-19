#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <stdlib.h>
#include <linux/input.h>
#include <linux/limits.h>
#include "tomlc99/toml.h"

// Some example code:
// http://sowerbutts.com/powermate/

// Settings
char *dev = "/dev/input/powermate";
char *knob_command = NULL;
char *long_press_command = NULL;
char *clock_wise_command = NULL;
char *counter_clock_wise_command = NULL;
int64_t long_press_ms = 1000;

// State
short muted = 0;
short movie_mode = 0;
short knob_depressed = 0;
int devfd = 0;
struct timeval knob_depressed_timestamp;

void exec_command(char *command) {
	if (command == NULL || command[0] == '\0') {
		return;
	}
	printf("Executing: %s\n", command);
	int ret = system(command);
	if (ret != 0) {
		printf("Return value: %d\n", ret);
	}
}

void set_led(unsigned int val) {
	// printf("set_led(%d)\n", val);
	struct input_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = EV_MSC;
	ev.code = MSC_PULSELED;
	ev.value = val;
	if (write(devfd, &ev, sizeof(ev)) != sizeof(ev)) {
		fprintf(stderr, "write(): %s\n", strerror(errno));
	}
}

void update_led(unsigned int val) {
	if (muted || movie_mode) {
		set_led(0);
	} else {
		set_led(val);
	}
}

char *get_config_home() {
	char *xdg_config_home = getenv("XDG_CONFIG_HOME");
	if (xdg_config_home != NULL && xdg_config_home[0] != '\0') {
		return xdg_config_home;
	}
	char *homedir = getenv("HOME");
	if (homedir == NULL) {
		return NULL;
	}
	char config_home[PATH_MAX] = "";
	snprintf(config_home, sizeof(config_home), "%s/.config", homedir);
	if (setenv("XDG_CONFIG_HOME", config_home, 1) != 0) {
		return NULL;
	}
	return getenv("XDG_CONFIG_HOME");
}

int main(int argc, char *argv[]) {
	for (int i = 1; i < argc; i++) {
		if ((strcmp(argv[i],"-c") && strcmp(argv[i],"-d"))     // check if it's an unexpected option
			|| (!strcmp(argv[i],"-c") && ++i == argc)             // check if it's -c but the filename is missing
			) {
			fprintf(stderr, "Usage: %s [-c file] [-d]\n", argv[0]);
			return 0;
		}
	}

	// Settings
	int daemonize = 0;

	// Load config file
	// TODO: split this into a separate function
	{
		char config_path[PATH_MAX] = "";
		for (int i=1; i < argc; i++) {
			if (!strcmp(argv[i], "-c")) {
				strcpy(config_path, argv[++i]);
				if (access(config_path, R_OK) != 0) {
					fprintf(stderr, "Could not access %s: %s\n", config_path, strerror(errno));
					return 1;
				}
			}
		}

		char *config_home = get_config_home();
		if (config_path[0] == '\0' && config_home != NULL) {
			snprintf(config_path, sizeof(config_path), "%s/powermate.toml", config_home);
			if (access(config_path, R_OK) != 0) {
				config_path[0] = '\0';
			}
		}
		if (config_path[0] == '\0') {
			strcpy(config_path, "/etc/powermate.toml");
		}

		if (access(config_path, R_OK) == 0) {
			printf("Loading config from %s\n", config_path);

			FILE *f;
			if ((f = fopen(config_path, "r")) == NULL) {
				fprintf(stderr, "Failed to open file.\n");
			}
			else {
				char errbuf[200];
				toml_table_t *conf = toml_parse_file(f, errbuf, sizeof(errbuf));
				fclose(f);
				if (conf == 0) {
					fprintf(stderr, "Error: %s\n", errbuf);
				}
				else {
					const char *raw;
					if ((raw=toml_raw_in(conf,"dev")) && toml_rtos(raw,&dev)) {
						fprintf(stderr, "Warning: bad value in 'dev', expected a string.\n");
					}
					if ((raw=toml_raw_in(conf,"daemonize")) && toml_rtob(raw,&daemonize)) {
						fprintf(stderr, "Warning: bad value in 'daemonize', expected a boolean.\n");
					}
					if ((raw=toml_raw_in(conf,"knob_command")) && toml_rtos(raw,&knob_command)) {
						fprintf(stderr, "Warning: bad value in 'knob_command', expected a string.\n");
					}
					if ((raw=toml_raw_in(conf,"long_press_command")) && toml_rtos(raw,&long_press_command)) {
						fprintf(stderr, "Warning: bad value in 'long_press_command', expected a string.\n");
					}
					if ((raw=toml_raw_in(conf,"clock_wise_command")) && toml_rtos(raw,&clock_wise_command)) {
						fprintf(stderr, "Warning: bad value in 'clock_wise_command', expected a string.\n");
					}
					if ((raw=toml_raw_in(conf,"counter_clock_wise_command")) && toml_rtos(raw,&counter_clock_wise_command)) {
						fprintf(stderr, "Warning: bad value in 'counter_clock_wise_command', expected a string.\n");
					}
					if ((raw=toml_raw_in(conf,"long_press_ms")) && toml_rtoi(raw,&long_press_ms)) {
						fprintf(stderr, "Warning: bad value in 'long_press_ms', expected an integer.\n");
					}
				}
				toml_free(conf);
			}
		} else {
			printf("Config file not found, using defaults. Checked the following paths:\n");
			if (config_home != NULL) {
				printf("- %s/powermate.toml\n", config_home);
			}
			printf("- /etc/powermate.toml\n");
			printf("\n");
		}
	}

	for (int i=1; i < argc; i++) {
		if (!strcmp(argv[i], "-d")) {
			daemonize = 1;
		}
	}

	// Test device
	devfd = open(dev, O_RDWR);
	if (devfd == -1) {
		fprintf(stderr, "Could not open %s: %s\n", dev, strerror(errno));
		fprintf(stderr, "Don't worry, it will be opened automatically if it appears.\n");
		fprintf(stderr, "If you just installed this program, you might have to unplug the device and then plug it back in..\n");
	}

	// Daemonize
	if (daemonize) {
		int pid = fork();
		if (pid == 0) {
			// We're the child process!
			// Release handle to the working directory
			if (chdir("/") < 0) {
				fprintf(stderr, "chdir() failed");
			}
			// Close things
			fclose(stdin);
			fclose(stdout);
			fclose(stderr);
		} else if (pid < 0) {
			fprintf(stderr, "Failed to become a daemon.\n");
		} else {
			printf("Just became a daemon.\n");
			return 0;
		}
	}

	struct pollfd fds[1];
	fds[0].fd = devfd;
	fds[0].events = POLLIN;
	fds[0].revents = 0;
	int timeout;

	// TODO: also split this into a separate function
	// TODO: CL port: https://github.com/joevandyk/Joe-s-Lisp-Exercises/blob/master/libraries/iolib/io.multiplex/fd-wait.lisp or call out to CL-IPC (https://common-lisp.net/project/cl-ipc/) using https://gitlab.common-lisp.net/cl-ipc/cl-ipc/blob/master/sample-client.c
	while (1) {
		//struct pollfd *pfds = malloc((nfds+1)*sizeof(struct pollfd));
		//memcpy(pfds, ufds, nfds*sizeof(struct pollfd));

		// wait for devfd
		while (devfd < 0) {
			fprintf(stderr, "Attempting to open %s\n", dev);
			devfd = open(dev, O_RDWR);
			if (devfd == -1) {
				fprintf(stderr, "Error: %s\n", strerror(errno));
				sleep(1);
			} else {
				printf("Device connected!\n");
				// When the device is connected, the kernel driver sets the LED to 50% brightness
				// We have to update the LED to represent the current volume
				update_led(sizeof(unsigned int) >> 1);
			}
		}

		// if the knob is depressed, then we need to timeout for the sake of detecting a long press
		if (knob_depressed && (long_press_command == NULL || long_press_command[0] != '\0')) {
			struct timeval now;
			if (gettimeofday(&now, NULL) < 0) {
				fprintf(stderr, "gettimeofday failed\n");
			}
			// setting timeout might be the reason we're seeing latency...
			timeout = (long_press_ms+knob_depressed_timestamp.tv_sec*1000+knob_depressed_timestamp.tv_usec/1000) - (now.tv_sec*1000+now.tv_usec/1000);
			// fprintf(stderr, "timeout=%d\n", timeout);
		} else {
			timeout = -1;
		}

		int ret = poll(fds, 1, timeout);
		if (ret < 0) {
			fprintf(stderr, "poll failed\n");
			exit(1);
		}

		/*
		int i;
		for (int i=0; i < nfds+1; i++) {
		  fprintf(stderr, "%d: fd: %d. events: %d. revents: %d.\n", i, pfds[i].fd, pfds[i].events, pfds[i].revents);
		}
		*/

		if (knob_depressed && ret == 0) {
			// timer ran out
			knob_depressed = 0;
			if (long_press_command == NULL) {
				movie_mode = !movie_mode;
				printf("Movie mode: %d\n", movie_mode);
			}
			else {
				exec_command(long_press_command);
			}
			update_led(sizeof(unsigned int) >> 1);
		}

		if (fds[0].revents > 0) {
			// fprintf(stderr, "fd: %d. events: %d. revents: %d.\n", pfds[nfds].fd, pfds[nfds].events, pfds[nfds].revents);
			struct input_event ev;
			int n = read(devfd, &ev, sizeof(ev));
			if (n != sizeof(ev)) {
				printf("Device disappeared!\n");
				devfd = -1;
			}
			else {
				if (ev.type == EV_REL && ev.code == 7) {
					if (ev.value == -1) {
						exec_command(counter_clock_wise_command);
					} else if (ev.value == 1) {
						exec_command(clock_wise_command);
					}
				}
				else if (ev.type == EV_KEY && ev.code == 256) {
					if (ev.value == 1) {
						// knob depressed
						knob_depressed = 1;
						knob_depressed_timestamp = ev.time;
					}
					else if (ev.value == 0 && knob_depressed) {
						// knob released
						knob_depressed = 0;
						exec_command(knob_command);
					}
				}
			}
		}
	}

	return 0;
}
