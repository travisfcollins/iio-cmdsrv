/* IIO Command Server
 *
 * Copyright 2012 Analog Devices Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <linux/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/dir.h>
#include <syslog.h>

#include "iio_utils.h"

#define CURR_VERSION	"0.3"
#define MAX_STR_LEN	1024
#define DBFS_REG_ATTR	"direct_reg_access"
#define REPORT_RETVAL(x) fprintf(stdout, "%d\n\n\n", x)

#define HELP_TEXT \
	"IIO Command Server Syntax:\n" \
	"read <IIODeviceName> <Attribute>\n" \
	"write <IIODeviceName> <Attribute> <Value>\n" \
	"readbuf <IIODeviceName> <NUMSamples> <BytesPerSample>\n" \
	"bufwrite <IIODeviceName> <NUMBytes>\n" \
	"sample <IIODeviceName> <NUMSamples> <BytesPerSample>\n" \
	"regread <IIODeviceName> <RegisterAddress>\n" \
	"regwrite <IIODeviceName> <RegisterAddress> <Value>\n" \
	"dbfsread <IIODeviceName> <Attribute>\n" \
	"dbfswrite <IIODeviceName> <Attribute> <Value>\n" \
	"show [<IIODeviceName> <Path>]\n" \
	"dbfsshow <IIODeviceName> <Path>\n" \
	"version\n"

char dev_dir_name[PATH_MAX];
char buf_dir_name[PATH_MAX];
char dbfs_dir_name[PATH_MAX];
char buffer_access[PATH_MAX];
char last_device_name[PATH_MAX];

// server:  nc.traditional -l -p 1234 -e main
// server:  while true; do nc -l -p 1234 -e iio_cmdsrv; done

//# mkfifo /tmp/foobar
//# while true; do cat /tmp/foobar | ./nc -l -p 1234 | /bin/iio_cmdsrv > /tmp/foobar ; done
//# busybox nc -l -l -p 1234 -e /bin/iio_cmdsrv

// client: echo -e  "read device attribute\n" | nc.traditional localhost 1234

enum {
	DO_READ,
	DO_WRITE,
	DO_SAMPLE,
	DO_READBUF,
	DO_WRITEBUF,
	DO_DBFSREGWRITE,
	DO_DBFSREGREAD,
	DO_SHOW,
	DO_DBFSWRITE,
	DO_DBFSREAD,
	DO_DBFSSHOW,
};

int set_dev_paths(char *device_name)
{
	int dev_num, ret;
	if (strncmp(device_name, last_device_name, PATH_MAX) != 0) {
	/* Find the device requested */
		dev_num = find_type_by_name(device_name, "iio:device");
		if (dev_num < 0) {
			syslog(LOG_ERR, "set_dev_paths failed to find the %s\n",
			       device_name);
			ret = -ENODEV;
			goto error_ret;
		}
		ret = snprintf(buf_dir_name, PATH_MAX,"%siio:device%d/buffer",
			       iio_dir, dev_num);
		if (ret >= PATH_MAX) {
			syslog(LOG_ERR, "set_dev_paths failed (%d)\n", __LINE__);
			ret = -EFAULT;
			goto error_ret;
		}
		snprintf(dev_dir_name, PATH_MAX, "%siio:device%d",
			 iio_dir, dev_num);
		snprintf(buffer_access, PATH_MAX, "/dev/iio:device%d",
			 dev_num);
		snprintf(dbfs_dir_name, PATH_MAX, "/sys/kernel/debug/iio/iio:device%d",
			 dev_num);

		strcpy(last_device_name, device_name);
	}

	ret = 0;
error_ret:
	return ret;
}

int read_devattr_stdout(char *basedir, char *filename)
{
	int ret = 0;
	FILE  *sysfsfp;
	char temp[PATH_MAX];
	char buf[MAX_STR_LEN];
	size_t len;

	ret = snprintf(temp, PATH_MAX, "%s/%s", basedir, filename);
	if (ret >= PATH_MAX) {
		syslog(LOG_ERR, "failed to generate file path\n");
		ret = -errno;
		goto error;
	}

	sysfsfp = fopen(temp, "r");
	if (sysfsfp == NULL) {
		syslog(LOG_ERR, "could not open file (%s)\n", temp);
		ret = -errno;
		goto error;
	}

	len = fread(buf, sizeof(char), MAX_STR_LEN, sysfsfp);
	if (len) {
		if(buf[len - 1] == '\n')
			buf[len - 1] = '\0';
		buf[len] = '\0';
		printf("0\n%s\n", buf);
	} else if (ferror(sysfsfp) && !feof(sysfsfp))  {
		printf("-1\n");
	}

	fclose(sysfsfp);
error:
	return ret;
}

int write_devattr(char *basedir, char *attr, char *str, char *str2)
{
	FILE  *sysfsfp;
	char temp[PATH_MAX];
	int ret;

	ret = snprintf(temp, PATH_MAX, "%s/%s", basedir, attr);
	if (ret >= PATH_MAX) {
		syslog(LOG_ERR, "failed to generate file path\n");
		ret = -errno;
		goto error;
	}

	sysfsfp = fopen(temp, "w");
	if (sysfsfp == NULL) {
		syslog(LOG_ERR, "could not open file (%s)\n", temp);
		ret = -errno;
		goto error;
	}
	if (str2 == NULL)
		ret = fprintf(sysfsfp, "%s\n", str);
	else
		ret = fprintf(sysfsfp, "%s %s\n", str, str2);

	if (ret < 0) {
		syslog(LOG_ERR, "write_devattr failed (%d)\n", __LINE__);
	}

	fclose(sysfsfp);

error:
	return ret;
}

int iio_writebuf(int samples)
{
	int ret, n, rc = 0;
	int fp;
	unsigned char *data;

	/* Setup ring buffer parameters */
	ret = write_sysfs_int("length", buf_dir_name, samples);
	if (ret < 0) {
		syslog(LOG_ERR, "write_sysfs_int failed (%d) (%d) %s\n",
			__LINE__, ret, buf_dir_name);
		goto error_ret;
	}

	data = malloc(samples);
	if (!data) {
		syslog(LOG_ERR, "malloc failed (%d)\n", __LINE__);
		ret = -ENOMEM;
		goto error_ret;
	}

	fp = open(buffer_access, O_WRONLY/* | O_NONBLOCK*/);
	if (fp < 0) {
		syslog(LOG_ERR, "Failed to open %s\n", buffer_access);
		ret = -errno;
		goto error_free_data;
	}

	do {
		n = fread(data + rc, 1, samples - rc, stdin);
		if (n > 0) {
			rc += n;
		} else if (errno == EAGAIN) {
			continue;
		} else {
			ret = -errno;
			goto error_close;
		}

	} while (rc < samples);

	ret = write(fp, data, samples);
	if (ret < 0)
		ret = -errno;

error_close:
	close(fp);
error_free_data:
	free(data);
error_ret:
	return ret;
}

int iio_sample(unsigned samples, unsigned bytes_per_scan)
{
	int ret, buf_len;
	int fp;
	unsigned short *data;

	buf_len = samples * bytes_per_scan;

	ret = read_sysfs_posint("enable", buf_dir_name);
	if (ret == 1) {
		write_sysfs_int("enable", buf_dir_name, 0);
		syslog(LOG_ERR, "read_sysfs_posint enable (%d)\n", ret);
	}

	/* Setup ring buffer parameters */
	ret = write_sysfs_int("length", buf_dir_name, buf_len);
	if (ret < 0) {
		syslog(LOG_ERR, "write_sysfs_int failed (%d) (%d) %s %d\n",
			__LINE__, ret, buf_dir_name, buf_len);
		REPORT_RETVAL(ret);
		fflush(stdout);
		goto error_ret;
	}

	/* Enable the buffer */
	ret = write_sysfs_int("enable", buf_dir_name, 1);
	if (ret < 0) {
		syslog(LOG_ERR, "write_sysfs_int failed (%d)\n", __LINE__);
	}

	data = malloc(buf_len);
	if (!data) {
		syslog(LOG_ERR, "malloc failed (%d)\n", __LINE__);
		ret = -ENOMEM;
		REPORT_RETVAL(ret);
		fflush(stdout);
		goto error_disable;
	}

	fp = open(buffer_access, O_RDONLY);
	if (fp < 0) {
		syslog(LOG_ERR, "Failed to open %s\n", buffer_access);
		ret = -errno;
		REPORT_RETVAL(ret);
		fflush(stdout);
		goto error_free_data;
	}

	ret = read(fp, data, buf_len);
	if (ret < 0)
		ret = -errno;

	REPORT_RETVAL(ret);
	fflush(stdout);

	if (ret > 0) {
		if (ret != buf_len)
			syslog(LOG_ERR, "short read (%d)\n", ret);
		fwrite(data, 1, buf_len, stdout);

	}

	close(fp);
error_free_data:
	free(data);
error_disable:
	/* Stop the ring buffer */
	ret = write_sysfs_int("enable", buf_dir_name, 0);
	if (ret < 0) {
		syslog(LOG_ERR, "write_sysfs_int failed (%d)\n", __LINE__);
	}
error_ret:
	return ret;
}

int iio_readbuf(unsigned samples, unsigned bytes_per_scan)
{
	int ret, buf_len;
	int fp;
	unsigned short *data;

	buf_len = samples * bytes_per_scan;

	data = malloc(buf_len);
	if (!data) {
		syslog(LOG_ERR, "malloc failed (%d)\n", __LINE__);
		ret = -ENOMEM;
		goto error_ret;
	}

	fp = open(buffer_access, O_RDONLY);
	if (fp < 0) {
		syslog(LOG_ERR, "Failed to open %s\n", buffer_access);
		ret = -errno;
		goto error_free_data;
	}

	ret = read(fp, data, buf_len);
	if (ret < 0)
		ret = -errno;

	REPORT_RETVAL(ret);
	fflush(stdout);

	if (ret > 0) {
		fwrite(data, 1, buf_len, stdout);
	}

	close(fp);
	free(data);
	return ret;

error_free_data:
	free(data);
error_ret:
	REPORT_RETVAL(ret);
	return ret;
}

int iio_show_devices(void)
{
	const struct dirent *ent;
	DIR *dp;
	int ret = 0, len, first = 1;
	FILE *sysfsfp;
	char filename[PATH_MAX];
	char buf[MAX_STR_LEN];


	dp = opendir(iio_dir);
	if (dp == NULL) {
		ret = -errno;
		goto error_ret;
	}

	while (ent = readdir(dp), ent != NULL) {
		ret = snprintf(filename, PATH_MAX, "%s%s/name", iio_dir, ent->d_name);
		if (ret >= PATH_MAX) {
			syslog(LOG_ERR, "set_dev_paths failed (%d)\n", __LINE__);
			ret = -EFAULT;
			goto error_close_dir;
		}

		if (strcmp(ent->d_name, ".") == 0)
			continue;
		if (strcmp(ent->d_name, "..") == 0)
			continue;

		sysfsfp = fopen(filename, "r");
		if (sysfsfp != NULL)
			if(fgets((char *) &buf, MAX_STR_LEN, sysfsfp) != NULL){
				if (first) {
					first = 0;
					printf("0\n");
				}

				len = strlen(buf);
				if(buf[len - 1] == '\n')
					buf[len - 1] = 0;

				printf("%s ", buf);
				fclose(sysfsfp);
			}
	}

	if (first)
		printf("-1\n");
	else
		printf("\n");

	fflush(stdout);
	closedir(dp);
	return 0;

error_close_dir:
	closedir(dp);
error_ret:
 REPORT_RETVAL(ret);
	return ret;
}

int iio_show_device_attributes(char *dir_name, char *attr)
{
	const struct dirent *ent;
	DIR *dp;
	int ret = 0, first = 1;

	if (attr == NULL) {
		dp = opendir(dir_name);
	} else {
		char path[PATH_MAX];
		ret = snprintf(path, PATH_MAX, "%s/%s", dir_name, attr);
		if (ret >= PATH_MAX) {
			syslog(LOG_ERR, "set_dev_paths failed (%d)\n", __LINE__);
			ret = -EFAULT;
			goto error_ret;
		}
		dp = opendir(path);
	}

	if (dp == NULL) {
		ret = -errno;
		goto error_ret;
	}

	while (ent = readdir(dp), ent != NULL) {
		if (ent->d_type == DT_REG) {
			if (first) {
				first = 0;
				printf("0\n");
			}
			printf("%s ", ent->d_name);
		}
	}

	if (first)
		printf("-1\n");
	else
		printf("\n");

	closedir(dp);
	return 0;

error_ret:
 REPORT_RETVAL(ret);
	return ret;
}

int main (void)
{
	char *line = NULL;
	size_t len = 0;
	ssize_t read;
	char *token, *saveptr1;
	char *device_name, *attr;
	unsigned int val, val2, cmd;
	int ret;

	while ((read = getline(&line, &len, stdin)) != -1) {
#ifdef DEBUG
		syslog(LOG_ERR, "%s\n", line);
#endif
		token = strtok_r(line, " ", &saveptr1);
		if (token == NULL)
			break;

		if (strncmp(token, "read", 4) == 0) {
			cmd = DO_READ;
		} else if (strncmp(token, "write", 5) == 0) {
			cmd = DO_WRITE;
		} else if (strncmp(token, "sample", 6) == 0) {
			cmd = DO_SAMPLE;
		} else if (strncmp(token, "bufread", 7) == 0) {
			cmd = DO_READBUF;
		} else if (strncmp(token, "bufwrite", 8) == 0) {
			cmd = DO_WRITEBUF;
		} else if (strncmp(token, "show", 4) == 0) {
			cmd = DO_SHOW;
		} else if (strncmp(token, "regwrite", 8) == 0) {
			cmd = DO_DBFSREGWRITE;
		} else if (strncmp(token, "regread", 7) == 0) {
			cmd = DO_DBFSREGREAD;
		} else if (strncmp(token, "dbfsshow", 8) == 0) {
			cmd = DO_DBFSSHOW;
		} else if (strncmp(token, "dbfswrite", 9) == 0) {
			cmd = DO_DBFSWRITE;
		} else if (strncmp(token, "dbfsread", 8) == 0) {
			cmd = DO_DBFSREAD;
		} else if (strncmp(token, "version", 7) == 0) {
			printf("%s\n", CURR_VERSION);
			fflush(stdout);
			continue;
		} else if (strncmp(token, "help", 4) == 0) {
			printf("%s\n", HELP_TEXT);
			fflush(stdout);
			continue;
		} else if (strncmp(token, "fru_eeprom", 10) == 0) {
			char *serial, *date;
			char filename[50];
			struct stat sts;

			serial = strtok_r(NULL, " \n", &saveptr1);
			snprintf(filename, 50, "/tmp/%s", serial);

			if (stat(filename, &sts) == -1 && errno == ENOENT) {
				date = strtok_r(NULL, " \n", &saveptr1);
				execlp("/bin/do_fru_eeprom.sh", "do_fru_eeprom.sh", serial, date, NULL);
			}

			if (line)
				free(line);


			exit(EXIT_SUCCESS);
		} else if (strncmp(token, "\n", 1) == 0) {
			fflush(stdout);
			continue;
		} else {
			break; /* EXIT */
		}

		device_name = strtok_r(NULL, " \n", &saveptr1);
		if ((device_name == NULL) && (cmd == DO_SHOW)) {
			iio_show_devices();
			continue;
		}

		if (device_name == NULL) {
			REPORT_RETVAL(-EINVAL);
			fflush(stdout);
			continue;
		}

		ret = set_dev_paths(device_name);
		if (ret < 0) {
			REPORT_RETVAL(ret);
			fflush(stdout);
			continue;
		}

		attr = strtok_r(NULL, " \n", &saveptr1);
		if (attr == NULL) {
			REPORT_RETVAL(-EINVAL);
			fflush(stdout);
			continue;
		}

		switch (cmd) {
			case DO_READ:
				read_devattr_stdout(dev_dir_name, attr);
				break;
			case DO_WRITE:
				token = strtok_r(NULL, " \n", &saveptr1);
				if (token)
					ret = write_devattr(dev_dir_name, attr, token, NULL);
				else
					ret = -EINVAL;

				REPORT_RETVAL(ret);
				break;
			case DO_SAMPLE:
				if (sscanf(attr, "%u", &val) == 1) {
					token = strtok_r(NULL, " \n", &saveptr1);
					if (token && (sscanf(token, "%u", &val2) == 1)) {
						iio_sample(val, val2);
						break;
					}
				}

				ret = -EINVAL;
				REPORT_RETVAL(ret);
				break;
			case DO_READBUF:
				if (sscanf(attr, "%u", &val) == 1) {
					token = strtok_r(NULL, " \n", &saveptr1);
					if (token && (sscanf(token, "%u", &val2) == 1)) {
						iio_readbuf(val, val2);
						break;
					}
				}

				ret = -EINVAL;
				REPORT_RETVAL(ret);
				break;
			case DO_WRITEBUF:
				if (sscanf(attr, "%u", &val) == 1)
					iio_writebuf(val);
				REPORT_RETVAL(ret);
				break;
			case DO_DBFSREGREAD:
				ret = write_devattr(dbfs_dir_name, DBFS_REG_ATTR, attr, NULL);
				if (ret < 0) {
					REPORT_RETVAL(ret);
					break;
				}
				read_devattr_stdout(dbfs_dir_name, DBFS_REG_ATTR);
				break;
			case DO_DBFSREGWRITE:
				token = strtok_r(NULL, " \n", &saveptr1);
				if (token)
					ret = write_devattr(dbfs_dir_name, DBFS_REG_ATTR, attr, token);
				else
					ret = -EINVAL;
				REPORT_RETVAL(ret);
				break;
			case DO_SHOW:
				iio_show_device_attributes(dev_dir_name, attr);
				break;
			case DO_DBFSSHOW:
				iio_show_device_attributes(dbfs_dir_name, attr);
				break;
			case DO_DBFSREAD:
				read_devattr_stdout(dbfs_dir_name, attr);
				break;
			case DO_DBFSWRITE:
				token = strtok_r(NULL, " \n", &saveptr1);
				if (token)
					ret = write_devattr(dbfs_dir_name, attr, token, NULL);
				else
					ret = -EINVAL;

				REPORT_RETVAL(ret);
				break;
			default:
				break;
		}

		fflush(stdout);
	}

	if (line)
		free(line);

	exit(EXIT_SUCCESS);
}
