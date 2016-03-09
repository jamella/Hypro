#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>

#include <ide.h>
#include <part.h>
#include <fs.h>
#include <ext4fs.h>
#include <mapmem.h>
#include <x86-common.h>
#include <kernel.h>
#include "devDriver.h"

static block_dev_desc_t *fs_dev_desc;
static disk_partition_t fs_partition;
static int fs_type = FS_TYPE_ANY;

//#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))

static inline int fs_probe_unsupported(block_dev_desc_t *fs_dev_desc, disk_partition_t *fs_partition)
{
    printf("** Unrecognized filesystem type **\n");
    return -1;
}

static inline int fs_ls_unsupported(const char *dirname)
{
    return -1;
}

static inline int fs_exists_unsupported(const char *filename)
{
    return 0;
}

static inline int fs_size_unsupported(const char *filename, loff_t *size)
{
    return -1;
}

static inline int fs_read_unsupported(const char *filename, void *buf, loff_t offset, loff_t len, loff_t *actread)
{
    return -1;
}

static inline int fs_write_unsupported(const char *filename, void *buf, loff_t offset, loff_t len, loff_t *actwrite)
{
    return -1;
}

static inline void fs_close_unsupported(void)
{
}

static inline int fs_uuid_unsupported(char *uuid_str)
{
    return -1;
}

struct fstype_info
{
    int fstype;
    char *name;
    /*
     * Is it legal to pass NULL as .probe()'s  fs_dev_desc parameter? This
     * should be false in most cases. For "virtual" filesystems which
     * aren't based on a U-Boot block device (e.g. sandbox), this can be
     * set to true. This should also be true for the dumm entry at the end
     * of fstypes[], since that is essentially a "virtual" (non-existent)
     * filesystem.
     */
    bool null_dev_desc_ok;
    int (*probe)(block_dev_desc_t *fs_dev_desc,
                 disk_partition_t *fs_partition);
    int (*ls)(const char *dirname);
    int (*exists)(const char *filename);
    int (*size)(const char *filename, loff_t *size);
    int (*read)(const char *filename, void *buf, loff_t offset, loff_t len, loff_t *actread);
    int (*write)(const char *filename, void *buf, loff_t offset, loff_t len, loff_t *actwrite);
    void (*close)(void);
    int (*uuid)(char *uuid_str);
};

static struct fstype_info fstypes[] =
{
    {
        .fstype = FS_TYPE_EXT,
        .name = "ext4",
        .null_dev_desc_ok = false,
        .probe = ext4fs_probe,
        .close = ext4fs_close,
        .ls = ext4fs_ls,
        .exists = ext4fs_exists,
        .size = ext4fs_size,
        .read = ext4_read_file,
        .write = ext4_write_file,
        .uuid = ext4fs_uuid,
    },
    {
        .fstype = FS_TYPE_ANY,
        .name = "unsupported",
        .null_dev_desc_ok = true,
        .probe = fs_probe_unsupported,
        .close = fs_close_unsupported,
        .ls = fs_ls_unsupported,
        .exists = fs_exists_unsupported,
        .size = fs_size_unsupported,
        .read = fs_read_unsupported,
        .write = fs_write_unsupported,
        .uuid = fs_uuid_unsupported,
    },
};

static struct fstype_info *fs_get_info(int fstype)
{
    struct fstype_info *info;
    int i;

    for (i = 0, info = fstypes; i < ARRAY_SIZE(fstypes) - 1; i++, info++)
    {
        if (fstype == info->fstype)
            return info;
    }

    /* Return the 'unsupported' sentinel */
    return info;
}

int fs_set_blk_dev(const char *ifname, const char *dev_part_str, int fstype)
{
    struct fstype_info *info;
    int part, i;

    part = get_device_and_partition(ifname, dev_part_str, &fs_dev_desc, &fs_partition, 1);
    if (part < 0)
        return -1;

    for (i = 0, info = fstypes; i < ARRAY_SIZE(fstypes); i++, info++)
    {
        if (fstype != FS_TYPE_ANY && info->fstype != FS_TYPE_ANY && fstype != info->fstype)
            continue;

        if (!fs_dev_desc && !info->null_dev_desc_ok)
            continue;

        if (!info->probe(fs_dev_desc, &fs_partition))
        {
            fs_type = info->fstype;
            return 0;
        }
    }

    return -1;
}

static void fs_close(void)
{
    struct fstype_info *info = fs_get_info(fs_type);

    info->close();

//    fs_type = FS_TYPE_ANY;
}

int fs_uuid(char *uuid_str)
{
    struct fstype_info *info = fs_get_info(fs_type);

    return info->uuid(uuid_str);
}

int fs_ls(const char *dirname)
{
    int ret;

    struct fstype_info *info = fs_get_info(fs_type);

    ret = info->ls(dirname);

//    fs_type = FS_TYPE_ANY;
//    fs_close();

    return ret;
}

int fs_exists(const char *filename)
{
    int ret;

    struct fstype_info *info = fs_get_info(fs_type);

    ret = info->exists(filename);

//    fs_close();

    return ret;
}

int fs_size(const char *filename, loff_t *size)
{
    int ret;

    struct fstype_info *info = fs_get_info(fs_type);

    ret = info->size(filename, size);

    fs_close();

    return ret;
}

int fs_read(const char *filename, ulong addr, loff_t offset, loff_t len, loff_t *actread)
{
    struct fstype_info *info = fs_get_info(fs_type);
    void *buf;
    int ret;

    /*
     * We don't actually know how many bytes are being read, since len==0
     * means read the whole file.
     */
    buf = map_sysmem(addr, len);
    ret = info->read(filename, buf, offset, len, actread);
    unmap_sysmem(buf);

    /* If we requested a specific number of bytes, check we got it */
    if (ret == 0 && len && *actread != len)
        printf("** %s shorter than offset + len **\n", filename);
    fs_close();

    return ret;
}

int fs_write(const char *filename, ulong addr, loff_t offset, loff_t len,
	     loff_t *actwrite)
{
    struct fstype_info *info = fs_get_info(fs_type);
    void *buf;
    int ret;

    buf = map_sysmem(addr, len);
    ret = info->write(filename, buf, offset, len, actwrite);
    unmap_sysmem(buf);

    if (ret < 0 && len != *actwrite)
    {
        printf("** Unable to write file %s **\n", filename);
        ret = -1;
    }
    fs_close();

    return ret;
}

int do_size(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[],
		int fstype)
{
    loff_t size;

    if (argc != 4)
        return CMD_RET_USAGE;

    if (fs_set_blk_dev(argv[1], argv[2], fstype))
        return 1;

    if (fs_size(argv[3], &size) < 0)
        return CMD_RET_FAILURE;

//	setenv_hex("filesize", size);

    return 0;
}

int do_load(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[],
		int fstype)
{
    unsigned long addr;
    const char *addr_str;
    const char *filename;
    loff_t bytes;
    loff_t pos;
    loff_t len_read;
    int ret;
    unsigned long time;
    char *ep;

    if (argc < 2)
        return CMD_RET_USAGE;
    if (argc > 7)
        return CMD_RET_USAGE;

    if (fs_set_blk_dev(argv[1], (argc >= 3) ? argv[2] : NULL, fstype))
        return 1;

    if (argc >= 4)
    {
        addr = strtoull(argv[3], &ep, 16);
        if (ep == argv[3] || *ep != '\0')
            return CMD_RET_USAGE;
    }
    else
    {
        addr_str = getenv("loadaddr");
        if (addr_str != NULL)
            addr = strtoull(addr_str, NULL, 16);
        else
            addr = CONFIG_SYS_LOAD_ADDR;
    }
    if (argc >= 5)
    {
        filename = argv[4];
    }
    else
    {
        filename = getenv("bootfile");
        if (!filename)
        {
            puts("** No boot file defined **\n");
            return 1;
        }
    }
    if (argc >= 6)
        bytes = strtoull(argv[5], NULL, 16);
    else
        bytes = 0;

    if (argc >= 7)
        pos = strtoull(argv[6], NULL, 16);
    else
        pos = 0;

//	time = get_timer(0);
    ret = fs_read(filename, addr, pos, bytes, &len_read);
//	time = get_timer(time);
    if (ret < 0)
        return 1;

/*	printf("%llu bytes read in %lu ms", (unsigned long long)len_read, time);
    if (time > 0) {
            puts(" (");
            print_size(div_u64(len_read, time) * 1000, "/s");
            puts(")");
    }
    puts("\n");

    setenv_hex("fileaddr", addr);
    setenv_hex("filesize", len_read);
*/
    return 0;
}

int do_ls(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[], int fstype)
{
    if (argc < 2)
        return CMD_RET_USAGE;
    if (argc > 4)
        return CMD_RET_USAGE;

    if (fs_set_blk_dev(argv[1], (argc >= 3) ? argv[2] : NULL, fstype))
        return 1;

    if (fs_ls(argc >= 4 ? argv[3] : "/"))
        return 1;

    return 0;
}

int file_exists(const char *dev_type, const char *dev_part, const char *file, int fstype)
{
    if (fs_set_blk_dev(dev_type, dev_part, fstype))
        return 0;

    return fs_exists(file);
}

int do_save(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[],
		int fstype)
{
    unsigned long addr;
    const char *filename;
    loff_t bytes;
    loff_t pos;
    loff_t len;
    int ret;
    unsigned long time;

    if (argc < 6 || argc > 7)
        return CMD_RET_USAGE;

    if (fs_set_blk_dev(argv[1], argv[2], fstype))
        return 1;

    addr = strtoull(argv[3], NULL, 16);
    filename = argv[4];
    bytes = strtoull(argv[5], NULL, 16);
    if (argc >= 7)
        pos = strtoull(argv[6], NULL, 16);
    else
        pos = 0;

//	time = get_timer(0);
    ret = fs_write(filename, addr, pos, bytes, &len);
//	time = get_timer(time);
    if (ret < 0)
        return 1;

/*	printf("%llu bytes written in %lu ms", (unsigned long long)len, time);
    if (time > 0) {
            puts(" (");
            print_size(div_u64(len, time) * 1000, "/s");
            puts(")");
    }
    puts("\n");
*/
    return 0;
}

int do_fs_uuid(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[],
		int fstype)
{
    int ret;
    char uuid[37];
    memset(uuid, 0, sizeof(uuid));

    if (argc < 3 || argc > 4)
        return CMD_RET_USAGE;

    if (fs_set_blk_dev(argv[1], argv[2], fstype))
        return 1;

    ret = fs_uuid(uuid);
    if (ret)
        return CMD_RET_FAILURE;

    if (argc == 4)
        setenv(argv[3], uuid);
    else
        printf("%s\n", uuid);

    return CMD_RET_SUCCESS;
}

#if 0
int do_fs_type(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	struct fstype_info *info;

	if (argc < 3 || argc > 4)
		return CMD_RET_USAGE;

	if (fs_set_blk_dev(argv[1], argv[2], FS_TYPE_ANY))
		return 1;

	info = fs_get_info(fs_type);

	if (argc == 4)
		setenv(argv[3], info->name);
	else
		printf("%s\n", info->name);

	return CMD_RET_SUCCESS;
}
#endif

disk_partition_t* getDiskPartition()
{
    return &fs_partition;
}

void fs_setFsType(block_dev_desc_t *devDesc, disk_partition_t *diskPart)
{
    struct fstype_info *info;
    int i;

    for (i = 0, info = fstypes; i < sizeof(fstypes)/sizeof(fstypes[0]); i++, info++)
    {
        if (info->fstype != FS_TYPE_ANY)
        {
            if (!info->probe(devDesc, diskPart))
            {
                fs_type = info->fstype;
                return 0;
            }
        }
    }

    return -1;
}

int fs_copy(char *srcFilePathName, char *dstFilePathName)
{
#define MAX_BUF_SIZE (DEFAULT_BLOCK_SIZE * 32)

    struct fstype_info *info;
    long dstFd;
    unsigned char buf[MAX_BUF_SIZE];
    loff_t readOffset = 0, fileSize, actRead;


    info = fs_get_info(fs_type);
    if(!info || !info->exists(srcFilePathName))
    {
        printf("%s doesn't exist.\n", srcFilePathName);
        return -1;
    }

    if(info->size(srcFilePathName, &fileSize) == 0 && fileSize == 0)
    {
        printf("%s file size is 0.\n", srcFilePathName);
        return -1;
    }

    dstFd = open(dstFilePathName, O_CREAT | O_EXCL | O_RDWR);
    if (dstFd == -1)
    {
        printf("create %s fail.\n", dstFilePathName);
        return -1;
    }

    while ( fileSize > 0 && info->read(srcFilePathName, buf, readOffset, MAX_BUF_SIZE, &actRead) == 0 && actRead > 0 )
    {
        write(dstFd, buf, actRead);
        readOffset += actRead;
        fileSize -= actRead;
    }

    close(dstFd);

    return 0;
}