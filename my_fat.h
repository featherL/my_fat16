//
// Created by xi4oyu on 6/3/21.
//

#ifndef MYFAT_MY_FAT_H
#define MYFAT_MY_FAT_H

#define FUSE_USE_VERSION 31

#include <fuse3/fuse.h>
#include <stdio.h>
#include <errno.h>
#include <stddef.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define META_READONLY       0b00000001
#define META_READ_WRITE     0b00000000
#define META_HIDDEN         0b00000010
#define META_SYSTEM         0b00000100
#define META_VOLUME_LABEL   0b00001000
#define META_DIRECTORY      0b00010000
#define META_ARCHIVE        0b00100000

#define BYTES_PER_SECTOR        512
#define NUMBER_OF_FAT           2
#define SECTORS_PER_CLUSTER     1
#define RESERVED_SECTOR         1
#define ROOT_ENTRIES            512
#define SECTORS_PER_FAT         9

// 一个簇的大小
#define CLUSTER_SIZE (BYTES_PER_SECTOR * SECTORS_PER_CLUSTER)

#define GET_

// 根目录扇区数
#define ROOT_SECTORS    ((ROOT_ENTRIES * sizeof(struct FCB) + BYTES_PER_SECTOR - 1) / BYTES_PER_SECTOR)

// 数据区扇区数
#define DATA_SECTORS    0x1000

// 除了数据区外，占用的扇区数
#define HEADER_SECTORS (ROOT_SECTORS + NUMBER_OF_FAT * SECTORS_PER_FAT + RESERVED_SECTOR)

// 虚拟磁盘空间大小
#define DRIVE_SIZE ((HEADER_SECTORS + DATA_SECTORS) * BYTES_PER_SECTOR)

// 文件删除标记
#define FILE_DELETE '\xe5'

struct options {
    const char *filename;
    int is_create;
    int show_help;
};

extern struct options opts;


//__attribute__((packed)) 表示结构体按字节对齐

struct BPB {
    uint16_t bytes_per_sector;          // 扇区字节数，通常为 512
    uint8_t sectors_per_cluster;        // 每簇扇区数
    uint16_t reserved_sector;           // 保留扇区数，第一个FAT开始之前的扇区数，包括引导扇区
    uint8_t number_of_fat;              // FAT 表数量，一般为 2
    uint16_t root_entries;              // 根目录项数
    uint16_t small_sector;              // 小扇区数，超出 16 位表示范围，则用 large_sector 表示本分区扇区数
    uint8_t media_descriptor;           // 媒体描述符
    uint16_t sectors_per_fat;           // 每个 FAT 占用的扇区数
    uint16_t sectors_per_trark;         // 每磁道扇区数
    uint16_t number_of_head;            // 磁头数
    uint32_t hidden_sector;             // 隐藏扇区数
    uint32_t large_sector;              // 大扇区数
}__attribute__((packed));

struct EBPB {
    uint8_t physical_drive_number;      // 物理驱动器号
    uint8_t reserved;                   // 保留，一般为 1
    uint8_t extended_boot_signature;    // 扩展引导标签
    uint16_t volume_serial_number;      // 卷序号
    char volume_label[11];           // 卷标
    char file_system_type[8];        // 文件系统类型，"FAT16"
}__attribute__((packed));

struct BootRecord {
    uint8_t jmp_boot[3];                // 跳转指令
    uint8_t oem_id[8];                  // OEM ID
    struct BPB bpb;
    struct EBPB ebpb;
    uint8_t boot_code[448];             // 引导程序代码
    uint32_t end_signature;             // 扇区结束标志 0xaa55
}__attribute__((packed));

// 未分配的簇
#define CLUSTER_FREE    0x0000

// 第一个可以用的簇号
#define CLUSTER_MIN 0x0002

// 最后一个可用的簇号
#define CLUSTER_MAX 0xFFEF

// 文件结束
#define CLUSTER_END   0xffff

// 目录表项
struct FCB {
    char filename[8];                // 文件名
    char extname[3];                 // 文件扩展名
    uint8_t metadata;                   // 属性
    uint8_t _[10];                      // 保留
    uint16_t last_modify_time;          // 文件最近修改时间
    uint16_t last_modify_date;          // 文件最近修改日期
    uint16_t first_cluster;             // 起始簇号
    uint32_t size;                      // 文件长度大小
}__attribute__((packed));

#define MAX_FILENAME sizeof(((struct FCB *)0)->filename)

// FAT 表项
struct FAT {
    uint16_t cluster;                   // 簇号
};

/**
 * 将一块内存区域格式化为 fat16 文件系统
 * @param addr 内存起始地址
 * @param size 内存区域大小
 * @return 成功返回 0，反之返回 -1
 */
int fat16_format(char *addr, int size);

/**
 * 读取文件/目录的内容
 * @param fcb 文件的 FCB 结构体指针
 * @param buff 保存读入数据的缓冲区
 * @param offset 读取的起点
 * @param length 读入数据的长度
 * @return 返回读入的数据长度,出错返回负值
 */
long long read_file(const struct FCB *fcb, void *buff, uint32_t offset, uint32_t length);

/**
 * 往文件写入数据
 * @param fcb 文件的 FCB 结构体指针
 * @param buff 待写入的数据
 * @param offset 写入数据的起始点
 * @param length 写入数据的长度
 * @return 返回写入的数据长度,出错返回负值
 */
long long write_file(struct FCB *fcb, const void *buff, uint32_t offset, uint32_t length);

/**
 * 查找文件或目录，需要注意的是，若路径是根目录需另行处理，该函数会返回 NULL
 * @param root 查找起始目录
 * @param entries 目录的项数
 * @param path 文件路径
 * @param error_code 错误码，为 0 表示无错误，找不到文件置为 -ENOENT，路径中间有非目录则置为 -ENOTDIR
 * @return 返回 FCB 控制块，找不到文件则返回 NULL
 */
struct FCB *find_file(struct FCB *root, uint32_t entries, const char *path, int *error_code);

/**
 * 获取文件的名称
 * @param file 文件的 FCB 结构体指针
 * @return 返回以 \0 结束的文件名字符串的指针（需要自行释放内存）
 */
char *get_filename(const struct FCB *file);

/**
 * 根据簇号，获取指向该簇的内存指针
 * @param cluster_num 簇号
 * @return 返回簇的起始地址，失败则返回 NULL
 */
char *get_cluster(uint32_t cluster_num);

/**
 * 判断该簇号是否在使用
 * @param cluster_num 簇号
 * @return 簇号在用返回 1，反之返回 0
 */
int is_cluster_inuse(uint32_t cluster_num);

/**
 * 判断该目录项是否是终止项
 * @param fcb 目录项的 FCB 结构体指针
 * @return 是则返回 1，反之返回 0
 */
int is_entry_end(const struct FCB *fcb);

/**
 * 判断该目录项是否存在
 * @param fcb 目录项的 FCB 结构体指针
 * @return 是则返回 1，反之返回 0
 */
int is_entry_exists(const struct FCB *fcb);

/**
 * 判断文件名是否合法
 * @param filename 文件名
 * @return 文件名符合规则返回 1，否则返回 0
 */
int is_filename_available(const char *filename);

/**
 * 获取一个可用的目录项用于存储
 * @param dir 目录
 * @param entries 目录最大项数
 * @return 成功返回目录项的 FCB 指针，反之返回 NULL
 */
struct FCB *get_free_entry(struct FCB *dir, uint32_t entries);

/**
 * 获取可用的簇，返回起始的簇号
 * @param count 分配多少个簇
 * @return 若为 CLUSTER_END 表示已无可用的簇号，反之返回起始的簇号
 */
uint16_t get_free_cluster_num(uint32_t count);

/**
 * 判断目录是否为空
 * @param dir 目录文件在父目录上的目录项(FCB) 指针
 * @return 是返回 1，反之返回 0
 */
int is_directory_empty(const struct FCB *file);

/**
 * 给文件新增一个簇
 * @param file 文件对应的 FCB 指针
 * @return 返回第一个簇的簇号，失败则返回 CLUSTER_END
 */
uint16_t file_new_cluster(struct FCB *file, uint32_t count);


/**
 * 删除文件
 * @param file 文件对应的 FCB 指针
 */
void remove_file(struct FCB *file);

/**
 * 释放占用的簇
 * @param first_num 文件的第一个簇号
 */
void release_cluster(uint32_t first_num);

/**
 * 获取文件占用的簇的数量
 * @param file 文件对应的 FCB 指针
 * @return 返回文件占用的簇的数量
 */
uint32_t get_cluster_count(const struct FCB *file);

// fuse {

void *my_init(struct fuse_conn_info *, struct fuse_config *);

int my_getattr(const char *, struct stat *, struct fuse_file_info *);

int my_readdir(const char *, void *, fuse_fill_dir_t, off_t, struct fuse_file_info *, enum fuse_readdir_flags);

int my_open(const char *, struct fuse_file_info *);

int my_create(const char *, mode_t, struct fuse_file_info *);

int my_unlink(const char *);

int my_read(const char *, char *, size_t, off_t, struct fuse_file_info *);

int my_write(const char *, const char *, size_t, off_t, struct fuse_file_info *);

int my_flush(const char *, struct fuse_file_info *);

int my_release(const char *, struct fuse_file_info *);

int my_truncate(const char *, off_t, struct fuse_file_info *);

int my_rename(const char *, const char *, unsigned int);

int my_chmod(const char *, mode_t, struct fuse_file_info *);

int my_chown(const char *, uid_t, gid_t, struct fuse_file_info *);

int my_statfs(const char *, struct statvfs *);

int my_opendir(const char *, struct fuse_file_info *);

int my_mkdir(const char *, mode_t);

int my_rmdir(const char *);

int my_releasedir(const char *, struct fuse_file_info *);

void my_destroy(void *);

int my_access(const char *, int);
// fuse }

#endif //MYFAT_MY_FAT_H
