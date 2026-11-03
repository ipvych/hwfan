#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <regex.h>
#include <sys/timerfd.h>

#define IS_SPECIAL_DOT_NAME(str) \
	(str[0] == '.' && (str[1] == '\0' || (str[1] == '.' && str[2] == '\0')))
#define ISSPACE(c) (c == ' ' || c == '\t')

typedef struct FdList {
	int fd;
	struct FdList *next;
} FdList;

static void fdlist_free(FdList *l) {
	if (!l) return;
	FdList *next;
	int tmp = errno; /* Preserve errno */
	while (1) {
		next = l->next;
		close(l->fd);
		free(l);
		if (!next) break;
		l = next;
	}
	errno = tmp;
}

#define MAX_ARG_LEN 128
#define MAX_STATES 16

typedef enum {
	CONFIG_OK = 0,
	CONFIG_NULL_IN_FILE,
	CONFIG_TOO_MANY_STATES,
	CONFIG_MISSING_ARG,
	CONFIG_ARG_TOO_LONG,
	CONFIG_EXTRA_ARG,
	CONFIG_INVALID_ARG,
	CONFIG_INVALID_DIRECTIVE,
	CONFIG_NAME_UNSET,
	CONFIG_SENSOR_UNSET,
	CONFIG_PWM_UNSET,
	CONFIG_STATES_UNSET
} ConfigErr;

struct ConfigState {
	int low;
	int high;
	int speed;
};

struct Config {
	char name[MAX_ARG_LEN];
	char sensor_regex[MAX_ARG_LEN];
	char pwm_regex[MAX_ARG_LEN];
	struct ConfigState states[MAX_STATES];
	int states_len;
	int lineno;
	int err;
};

static char* nexttok(char *str, char **saveptr, size_t *lenp) {
	if (*saveptr != NULL)
		str = *saveptr;
	while (*str && ISSPACE(*str))
		str++;
	if (*str == '\0' || *str == '#')
		return NULL;

	size_t len = 0;
	char *ptr = str;
	while (*ptr && !ISSPACE(*ptr) && *ptr != '#') {
		len++;
		ptr++;
	}
	if (*ptr == '#') {
		/* Storing \0 in saveptr makes next call return NULL when checking for
		 * empty string above. */
		*ptr = '\0';
	} else if (ISSPACE(*ptr)) {
		*ptr = '\0';
		ptr++;
	}
	*saveptr = ptr;
	if (lenp)
		*lenp = len;
	return str;
}

static int read_config(FILE *f, struct Config *cfg) {
	int rc = 0;
	char *lineptr = NULL, *saveptr, *token;
	size_t toklen, linesize;
	ssize_t nread;
	cfg->lineno = 0;
	cfg->name[0] = '\0';
	cfg->sensor_regex[0] = '\0';
	cfg->pwm_regex[0] = '\0';
	cfg->states_len = 0;
	while((nread = getline(&lineptr, &linesize, f)) != -1) {
		cfg->lineno++;
		/* Bail on embedded NULL's */
		for (ssize_t i = 0; i < nread; i++) {
			if (lineptr[i] == '\0') {
				cfg->err = CONFIG_NULL_IN_FILE;
				goto err;
			}
		}
		/* Don't need terminating newline for splitting tokens */
		lineptr[nread-1] = '\0';

		saveptr = NULL;
		/* First call will return NULL when line is empty or a comment */
		token = nexttok(lineptr, &saveptr, NULL);
		if (token == NULL)
			continue;
		if (strcmp(token, "name") == 0) {
			token = nexttok(NULL, &saveptr, &toklen);
			if (token == NULL) {
				cfg->err = CONFIG_MISSING_ARG;
				goto err;
			}
			if (toklen > MAX_ARG_LEN) {
				cfg->err = CONFIG_ARG_TOO_LONG;
				goto err;
			}
			memcpy(cfg->name, token, toklen+1);
		} else if (strcmp(token, "sensor_regex") == 0) {
			token = nexttok(NULL, &saveptr, &toklen);
			if (token == NULL) {
				cfg->err = CONFIG_MISSING_ARG;
				goto err;
			}
			if (toklen > MAX_ARG_LEN) {
				cfg->err = CONFIG_ARG_TOO_LONG;
				goto err;
			}
			memcpy(cfg->sensor_regex, token, toklen+1);
		} else if (strcmp(token, "pwm_regex") == 0) {
			token = nexttok(NULL, &saveptr, &toklen);
			if (token == NULL) {
				cfg->err = CONFIG_MISSING_ARG;
				goto err;
			}
			if (toklen > MAX_ARG_LEN) {
				cfg->err = CONFIG_ARG_TOO_LONG;
				goto err;
			}
			memcpy(cfg->pwm_regex, token, toklen+1);
		} else if (strcmp(token, "state") == 0) {
			if (cfg->states_len >= MAX_STATES) {
				cfg->err = CONFIG_TOO_MANY_STATES;
				goto err;
			}
			int *states[3] = {
				&cfg->states[cfg->states_len].speed,
				&cfg->states[cfg->states_len].low,
				&cfg->states[cfg->states_len].high
			};
			for (int i = 0; i < 3; i++) {
				token = nexttok(NULL, &saveptr, &toklen);
				if (token == NULL) {
					cfg->err = CONFIG_MISSING_ARG;
					goto err;
				}
				if (toklen > MAX_ARG_LEN) {
					cfg->err = CONFIG_ARG_TOO_LONG;
					goto err;
				}
				char *endptr;
				int v = (int)strtol(token, &endptr, 10);
				if (endptr == token || *endptr != '\0') {
					cfg->err = CONFIG_INVALID_ARG;
					goto err;
				}
				*(states[i]) = v;
			}
			cfg->states_len++;
		} else {
			cfg->err = CONFIG_INVALID_DIRECTIVE;
			goto err;
		}

		if (nexttok(NULL, &saveptr, NULL) != NULL) {
			cfg->err = CONFIG_EXTRA_ARG;
			goto err;
		}
	}
	goto exit;
err:
	rc = -1;
exit:
	free(lineptr);
	if (cfg->name[0] == '\0')
		return CONFIG_NAME_UNSET;
	if (cfg->sensor_regex[0] == '\0')
		return CONFIG_SENSOR_UNSET;
	if (cfg->pwm_regex[0] == '\0')
		return CONFIG_PWM_UNSET;
	if (cfg->states_len < 2)
		return CONFIG_STATES_UNSET;
	return rc;
}

static DIR* open_hwmon_dir(const char *hwmon_dir_path, char *name) {
	DIR *ret_dirp = NULL;
	DIR *dirp;
	int dir_fd;
	if ((dir_fd = openat(0, hwmon_dir_path, 0)) == -1)
		return NULL;
	if ((dirp = fdopendir(dir_fd)) == NULL) {
		close(dir_fd);
		return NULL;
	}

	char path_buf[(sizeof("hwmon") - 1) + 4 + sizeof("/name")];
	char name_buf[MAX_ARG_LEN];
	int name_fd, file_fd;
	ssize_t nread;
	for (struct dirent *d = readdir(dirp); d; d = readdir(dirp)) {
		if (IS_SPECIAL_DOT_NAME(d->d_name))
			continue;

		if (snprintf(path_buf, sizeof(path_buf)-1, "%s/name", d->d_name) >= (int)(sizeof(path_buf)-1))
			continue;
		name_fd = openat(dir_fd, path_buf, O_RDONLY);
		if (name_fd == -1)
			continue;
		nread = read(name_fd, name_buf, sizeof(name_buf));
		close(name_fd);
		if (nread == -1
			|| nread == 0
			|| strncmp(name_buf, name, nread-1) != 0
			|| name_buf[nread-1] != '\n'
		) {
			continue;
		}

		file_fd = openat(dir_fd, d->d_name, 0);
		if (file_fd == -1)
			continue;
		ret_dirp = fdopendir(file_fd);
		break;
	}
	int tmp = errno; /* Preserve errno */
	closedir(dirp);
	close(dir_fd);
	errno = tmp;
	return ret_dirp;
}

static int open_control_fds(
	DIR *hwmon_dirp,
	const char *pwm_regex,    FdList **pwm_fds,
	const char *sensor_regex, FdList **sensor_fds
) {
	if (!hwmon_dirp || !pwm_regex || !pwm_fds || !sensor_regex || !sensor_fds) {
		errno = EINVAL;
		return -1;
	}
	regex_t pwm_regp, sensor_regp;
	if (regcomp(&pwm_regp, pwm_regex, REG_EXTENDED | REG_NOSUB) != 0)
		return -1;
	if (regcomp(&sensor_regp, sensor_regex, REG_EXTENDED | REG_NOSUB) != 0){
		regfree(&pwm_regp);
		return -1;
	}

	int tmp_errno;
	int rc = 0;
	int file_fd = -1, dir_fd = dirfd(hwmon_dirp);
	FdList *head_pwm = NULL, *head_sensor = NULL,
		*cur_pwm = NULL, *cur_sensor = NULL,
		*new;
	for (struct dirent *d = readdir(hwmon_dirp); d; d = readdir(hwmon_dirp)) {
		if (IS_SPECIAL_DOT_NAME(d->d_name))
			continue;
		if (regexec(&pwm_regp, d->d_name, 0, NULL, 0) == 0) {
			file_fd = openat(dir_fd, d->d_name, O_RDWR);
			if (file_fd == -1)
				goto err;
			new = malloc(sizeof(FdList));
			if (new == NULL) {
				close(file_fd);
				goto err;
			}
			new->fd = file_fd;
			new->next = NULL;
			if (head_pwm == NULL) {
				head_pwm = new;
				cur_pwm = head_pwm;
			} else {
				cur_pwm->next = new;
				cur_pwm = new;
			}
		}
		if (regexec(&sensor_regp, d->d_name, 0, NULL, 0) == 0) {
			file_fd = openat(dir_fd, d->d_name, O_RDONLY);
			if (file_fd == -1)
				goto err;
			new = malloc(sizeof(FdList));
			if (new == NULL) {
				close(file_fd);
				goto err;
			}
			new->fd = file_fd;
			new->next = NULL;
			if (head_sensor == NULL) {
				head_sensor = new;
				cur_sensor = head_sensor;
			} else {
				cur_sensor->next = new;
				cur_sensor = new;
			}
		}
	}
	if (!head_pwm || !head_sensor)
		goto err;
	goto exit;
err:
	rc = -1;
	fdlist_free(head_pwm);
	fdlist_free(head_sensor);
exit:
	/* free_fdlist preserves errno; idk about regfree so I preserve it myself. */
	tmp_errno = errno;
	regfree(&pwm_regp);
	regfree(&sensor_regp);
	*pwm_fds = head_pwm;
	*sensor_fds = head_sensor;
	errno = tmp_errno;
	return rc;
}

static int read_long(int fd, long *out) {
	char buf[16];
	ssize_t nread = read(fd, buf, sizeof(buf));
	if (nread == -1 || nread == 0)
		return -1;
	buf[nread-1] = '\0'; /* Strip newline */
	char *endptr;
	*out = strtol(buf, &endptr, 10);
	if (endptr == buf || *endptr != '\0')
		return -1;
	return 0;
}

#define DEFAULT_CONFIG_PATH "/etc/hwfan.conf"
#define HWMON_PATH "/sys/class/hwmon"
int main(int argc, char **argv) {
	(void)argc;
	(void)argv;
	struct Config cfg;
	FILE *f = fopen(DEFAULT_CONFIG_PATH, "r");
	if (f == NULL) {
		perror("failed opening config file");
		return -1;
	}
	if (read_config(f, &cfg) != CONFIG_OK) {
		switch (cfg.err) {
		case CONFIG_NULL_IN_FILE:
			fprintf(stderr, "error: null character at line %d\n", cfg.lineno);
			break;
		case CONFIG_TOO_MANY_STATES:
			fprintf(stderr, "error: too many state definitions at line %d\n", cfg.lineno);
			break;
		case CONFIG_MISSING_ARG:
			fprintf(stderr, "error: missing argument at line %d\n", cfg.lineno);
			break;
		case CONFIG_ARG_TOO_LONG:
			fprintf(stderr, "error: argument too long at line %d\n", cfg.lineno);
			break;
		case CONFIG_EXTRA_ARG:
			fprintf(stderr, "error: extra argument at line %d\n", cfg.lineno);
			break;
		case CONFIG_INVALID_ARG:
			fprintf(stderr, "error: argument is invalid at line %d\n", cfg.lineno);
			break;
		case CONFIG_INVALID_DIRECTIVE:
			fprintf(stderr, "error: invalid directive at line %d\n", cfg.lineno);
			break;
		case CONFIG_NAME_UNSET:
			fprintf(stderr, "error: name is not set in config\n");
			break;
		case CONFIG_SENSOR_UNSET:
			fprintf(stderr, "error: sensor_regex is not set in config\n");
			break;
		case CONFIG_PWM_UNSET:
			fprintf(stderr, "error: pwm_regex is not set in config\n");
			break;
		case CONFIG_STATES_UNSET:
			fprintf(stderr, "error: states are not set in config\n");
			break;
		}
		return 1;
	}

	DIR* hwmon_dir = open_hwmon_dir(HWMON_PATH, cfg.name);
	if (!hwmon_dir) {
		perror("hwmon_dir");
		return 1;
	}
	FdList *pwm_fds, *sensor_fds;
	if (open_control_fds(hwmon_dir, cfg.pwm_regex, &pwm_fds, cfg.sensor_regex, &sensor_fds) == -1) {
		perror("open fds");
		return 1;
	}
	closedir(hwmon_dir);

	/* Timerfd armed for CLOCK_BOOTTIME is used to be able to tell when the
	 * system wakes up from sleep, otherwise plain sleep(1) would work.
	 * As alternative I could read fan speeds continuously and adjust speeds
	 * that way but on my system (Dell XPS 15 9550) doing that triggers BIOS
	 * interrupts that cause audio dropouts so I have to do it this way. */
	int tfd = timerfd_create(CLOCK_BOOTTIME, 0);
	if (tfd == -1){
		perror("timerfd_create");
		return 1;
	}
	const struct itimerspec spec = {
		.it_value = (struct timespec){ .tv_sec = 1 },
		.it_interval = (struct timespec){ .tv_sec = 1 },
	};
	if (timerfd_settime(tfd, 0, &spec, NULL) == -1) {
		perror("timerfd_settime");
		return 1;
	}
	uint64_t timebuf;
	int config_n = -1; /* -1 is special and forces fan speed to be updated */
	FdList *p;
	while (1) {
		long num;
		int max_temp = 0;
		p = sensor_fds;
		do {
			if (read_long(p->fd, &num) == -1)
				/* Bail early as this can't happen with valid config */
				return 1;
			if (num > max_temp)
				max_temp = num;
			lseek(p->fd, 0, SEEK_SET);
		} while((p = p->next));
		max_temp /= 1000; /* Temp is stored in millidegrees */

		int i = config_n == -1 ? 0 : config_n;
		while (i < cfg.states_len-1 && max_temp >= cfg.states[i].high)
			i++;
		while (i > 0 && max_temp <= cfg.states[i].low)
			i--;
		if (config_n == -1 || i != config_n) {
			char buf[7]; /* 6 bytes for number + 1 for newline. */
			int len = snprintf(buf, sizeof(buf), "%d\n", cfg.states[i].speed);
			if (len == -1)
				return 1;
			p = pwm_fds;
			do {
				if (write(p->fd, buf, len) == -1)
					/* Bail early as this can't happen with valid config */
					return 1;
			} while((p = p->next));
			config_n = i;
		}
		if (read(tfd, &timebuf, sizeof(timebuf)) == -1) {
			perror("read tfd");
			return 1;
		}
		if (timebuf > 2) /* Woke up from suspend - update timer speeds */
			config_n = -1;
	}
}
