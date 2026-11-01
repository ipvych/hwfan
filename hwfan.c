#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <regex.h>

#define LENGTH(x) (sizeof (x) / sizeof (x)[0])
#define IS_SPECIAL_DOT_NAME(str) \
	(str[0] == '.' && (str[1] == '\0' || (str[1] == '.' && str[2] == '\0')))

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
	/* 64 bytes should be more than enough.
	 * TODO: When writing config parser should limit name length there */
	char name_buf[64];
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

struct ConfigEntry {
	int speed;
	int low;
	int high;
};

enum {
	SPEED_OFF = 0,
	SPEED_LOW = 128,
	SPEED_MAX = 255,
};

static const struct ConfigEntry config[] = {
	{SPEED_OFF, -1, 65},
	{SPEED_LOW, 45, 75},
	{SPEED_MAX, 60, 128},
};
static const int config_len = LENGTH(config);

/* TODO: Document that regexp matching is greedy when writing config parser. */
#define INPUT_RE "^temp[0-9]+_input$"
#define PWM_RE "^pwm[0-9]+$"
#define HWMON_PATH "/sys/class/hwmon"
#define DELL_SMM_NAME "dell_smm"
int main(int argc, char **argv) {
	(void)argc;
	(void)argv;
	DIR* hwmon_dir = open_hwmon_dir(HWMON_PATH, DELL_SMM_NAME);
	if (!hwmon_dir) {
		perror("hwmon_dir");
		return 1;
	}
	FdList *pwm_fds, *sensor_fds;
	if (open_control_fds(hwmon_dir, PWM_RE, &pwm_fds, INPUT_RE, &sensor_fds) == -1) {
		perror("open fds");
		return 1;
	}
	closedir(hwmon_dir);

	int config_n = 0;
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

		int i = config_n;
		while (i < config_len-1 && max_temp >= config[i].high)
			i++;
		while (i > 0 && max_temp <= config[i].low)
			i--;
		if (i != config_n) {
			char buf[7]; /* 6 bytes for number + 1 for newline. */
			int len = snprintf(buf, sizeof(buf), "%d\n", config[i].speed);
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
		sleep(1);
	}
}
