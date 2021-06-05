
#include "my_fat.h"

static void show_help(const char *progname)
{
    printf("usage: %s [options] <mountpoint>\n\n", progname);
    printf("FileSystem Options: \n");
    printf("--name filename to store data\n");
    printf("-ct create a new file to store data");
}

#define OPTION(t, p)                           \
    { t, offsetof(struct options, p), 1 }

static const struct fuse_opt option_spec[] = {
        OPTION("-ct", is_create),
        OPTION("--name=%s", filename),
        OPTION("-h", show_help),
        OPTION("--help", show_help),
        FUSE_OPT_END
};

static const struct fuse_operations my_fat_ops = {
    .init = my_init,
    .getattr = my_getattr,
    .readdir = my_readdir,
    .open = my_open,
    .create = my_create,
    .unlink = my_unlink,
    .read = my_read,
    .write = my_write,
    .flush = my_flush,
//    .release = my_release,
//    .truncate = my_truncate,
//    .rename = my_rename,
    .chmod = my_chmod,
    .chown = my_chown,
    .statfs = my_statfs,
    .opendir = my_opendir,
    .mkdir = my_mkdir,
    .rmdir = my_rmdir,
//    .releasedir = my_releasedir,
    .destroy = my_destroy,
    .access = my_access,
};


int main(int argc, char *argv[])
{
    int ret;
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    if (fuse_opt_parse(&args, &opts, option_spec, NULL) == -1)
        return 1;

    if (opts.show_help) {
        show_help(argv[0]);
        assert(fuse_opt_add_arg(&args, "--help") == 0);
        args.argv[0][0] = '\0';
    }

    ret = fuse_main(args.argc, args.argv, &my_fat_ops, NULL);
    fuse_opt_free_args(&args);

    return ret;
}
