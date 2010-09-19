/* (C) 2006-2010 ZC Miao <hellwolf.misty@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include "config.h"

#define FUSE_USE_VERSION 25
#include <fuse.h>
#include <fuse_opt.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <iconv.h>
#include <pthread.h>

#if HAVE_ATTR_XATTR_H
#include <attr/xattr.h>
#endif

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <cassert>
#include <string>


using namespace std;

/*
 * global vars
 */
#define FUSE_CONVMVFS_MAGIC 0x4064
static const char* CONVMVFS_DEFAULT_SRCDIR = "/"; /* root dir */
static const char* CONVMVFS_DEFAULT_ICHARSET = "UTF-8";
static const char* CONVMVFS_DEFAULT_OCHARSET = "UTF-8";

struct convmvfs {
  const char *cwd;
  const char *srcdir;
  const char *icharset;
  const char *ocharset;
};
static struct convmvfs convmvfs;

static uid_t euid;
gid_t egid;

static void init_gvars(){
  convmvfs.cwd = get_current_dir_name();
  convmvfs.srcdir = CONVMVFS_DEFAULT_SRCDIR;
  convmvfs.icharset = CONVMVFS_DEFAULT_ICHARSET;
  convmvfs.ocharset =  CONVMVFS_DEFAULT_OCHARSET;

  euid = geteuid();
  egid = getegid();
}

static struct fuse_operations convmvfs_oper;

static iconv_t ic_out2in,ic_in2out;
static pthread_mutex_t iconv_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * options and usage
 */
enum {
  KEY_HELP,
  KEY_VERSION,
};

#define CONVMVFS_OPT(t, p, v) { t, offsetof(struct convmvfs, p), v }

static struct fuse_opt convmvfs_opts[] = {
  CONVMVFS_OPT("srcdir=%s", srcdir, 0),
  CONVMVFS_OPT("icharset=%s", icharset, 0),
  CONVMVFS_OPT("ocharset=%s", ocharset, 0),

  FUSE_OPT_KEY("-V",        KEY_VERSION),
  FUSE_OPT_KEY("--version", KEY_VERSION),
  FUSE_OPT_KEY("-h",        KEY_HELP),
  FUSE_OPT_KEY("--help",    KEY_HELP),
  {NULL,0,0}
};

static void usage(){
  printf(
         "CONVMVFS options:\n"
         "    -o srcdir=PATH         which directory to convert\n"
         "    -o icharset=CHARSET    charset used in srcdir\n"
         "    -o ocharset=CHARSET    charset used in mounted filesystem\n"
         );
}

static int convmvfs_opt_proc(void *data, const char *arg,int key,
                          struct fuse_args *outargs)
{
  (void)arg;
  (void)data;
  
  switch (key) {
  case FUSE_OPT_KEY_OPT:
  case FUSE_OPT_KEY_NONOPT:
    return 1;
  case KEY_VERSION:
    fprintf(stderr, PACKAGE"\t"VERSION"\n"
            "Copyright (C) 2006 ZC Miao <hellwolf@seu.edu.cn>\n\n"
            "This program is free software; you can redistribute it and/or modify\n"
            "it under the terms of the GNU General Public License version 2 as\n"
            "published by the Free Software Foundation.\n");
    exit(0);
  case KEY_HELP:
    fuse_opt_add_arg(outargs, "-h");
    fuse_main(outargs->argc, outargs->argv, &convmvfs_oper);
    usage();
    exit(0);
  default:
    fprintf(stderr, "unknown option\n");
    exit(1);
  }
}



/*
 * util funs
 */
#define OUTINBUFLEN 255
static string outinconv(const char* s, const iconv_t ic){
  char buf[OUTINBUFLEN];
  char* inbuf((char*)s);
  char * outbuf(buf);
  size_t ibleft(strlen(inbuf)),obleft(OUTINBUFLEN);
  string res;

  do{
    pthread_mutex_lock(&iconv_mutex);
    size_t niconv = iconv(ic,
                   &inbuf,&ibleft,
                   &outbuf,&obleft);
    pthread_mutex_unlock(&iconv_mutex);
    if ( niconv == (size_t) -1 ){
      switch(errno){
      case EINVAL:
      case EILSEQ:
        return res + string(buf, OUTINBUFLEN - obleft) + "???";
        break;
      case E2BIG:
        res += string(buf,OUTINBUFLEN - obleft);
        outbuf = buf;
        obleft = OUTINBUFLEN;
        continue;
      default:
        return "????";
      }
    }
    res += string(buf,OUTINBUFLEN - obleft);
    break;
  }while(1);
  return res;
}

inline
static string out2in(const char* s){
  return outinconv(s,ic_out2in);
}

inline
static string in2out(const char* s){
  return outinconv(s,ic_in2out);
}


#define PERM_WALK_CHECK_READ   01
#define PERM_WALK_CHECK_WRITE  02
#define PERM_WALK_CHECK_EXEC   04
static int permission_walk(const char *path, uid_t uid, gid_t gid,
                           int perm_chk, int readlink = 0){
  int rt;
  //I'm root~~
  if(uid == 0){
    return 0;
  }
  size_t l = strlen(path) + 1;
  char *p = (char*)malloc(l);
  if(p == NULL){
    return -ENOMEM;
  }
  strncpy(p, path, l);

  char *s = p;
  if(*s == '\0'){
    //Empty pathname, see PATH_RESOLUTION(2)
    rt = -ENOENT;
    goto __free_quit;
  }
  while(*s++){
    struct stat stbuf;
    int chk;
    if(*s == '\0'){
      //final entry
      if(readlink?lstat(p, &stbuf):stat(p, &stbuf)){
        rt = -errno;
        goto __free_quit;
      }
      chk = perm_chk;
    }else if(*s == '/'){
      //non-final component
      *s = '\0';
      if(stat(p, &stbuf)){
        rt =  -errno;
        goto __free_quit;
      }
      if(!(stbuf.st_mode & S_IFDIR)){
        rt = -ENOTDIR;
        goto __free_quit;
      }
      *s = '/';
      chk = PERM_WALK_CHECK_EXEC;
    }else{
      continue;
    }
    int mr,mw,mx;
    if(stbuf.st_uid == uid){
      mr = S_IRUSR;
      mw = S_IWUSR;
      mx = S_IXUSR;
    }else if(stbuf.st_gid == gid){
      mr = S_IRGRP;
      mw = S_IWGRP;
      mx = S_IXGRP;
    }else{
      mr = S_IROTH;
      mw = S_IWOTH;
      mx = S_IXOTH;
    }
    if(chk & PERM_WALK_CHECK_READ){
      if(!(stbuf.st_mode & mr)){
        rt = -EACCES;
        goto __free_quit;
      }
    }
    if(chk & PERM_WALK_CHECK_WRITE){
      if(!(stbuf.st_mode & mw)){
        rt = -EACCES;
        goto __free_quit;
      }
    }
    if(chk & PERM_WALK_CHECK_EXEC){
      if(!(stbuf.st_mode & mx)){
        rt = -EACCES;
        goto __free_quit;
      }
    }
  }

  rt = 0;
__free_quit:
  free(p);
  return rt;
}

static int permission_walk_parent(const char *path, uid_t uid, gid_t gid,
                                  int perm_chk){
  int l = strlen(path);
  while(--l)
    if(path[l] == '/')
      break;
  return permission_walk(string(path, l).c_str(), uid, gid,
                         perm_chk);
}


/*
 * opers
 */
static void *convmvfs_init(void){
  if(chdir(convmvfs.cwd)){
    perror("fuse init,chdir failed");
    exit(errno);
  }
  return NULL;
}

static int convmvfs_open(const char *opath, struct fuse_file_info *fi){
  string ipath = convmvfs.srcdir + out2in(opath);

  /* permission check*/
  struct fuse_context *cont = fuse_get_context();
  int st;
  if(fi->flags & O_WRONLY){
    st = permission_walk(ipath.c_str(), cont->uid, cont->gid,
                           PERM_WALK_CHECK_WRITE);
  }else if(fi->flags & O_RDWR){
    st = permission_walk(ipath.c_str(), cont->uid, cont->gid,
                         PERM_WALK_CHECK_READ|PERM_WALK_CHECK_WRITE);
  }else{
    st = permission_walk(ipath.c_str(), cont->uid, cont->gid,
                         PERM_WALK_CHECK_READ);
  }
  if(st)
    return st;

  int fd;
  if( ( fd = open(ipath.c_str(),fi->flags)) == -1 ){
    return -errno;
  }
  fi->fh = fd;

  return 0;
}

static int convmvfs_read(const char *opath, char *buf,
                         size_t size, off_t offset,
                         struct fuse_file_info *fi){
  (void)opath;

  lseek(fi->fh, offset, SEEK_SET);
  return read(fi->fh, buf, size);
}

static int convmvfs_write(const char *opath, const char *buf, size_t size, off_t off, struct fuse_file_info *fi){
  (void)opath;

  lseek(fi->fh, off, SEEK_SET);
  return write(fi->fh, buf, size);
}

static int convmvfs_release(const char *opath, struct fuse_file_info *fi){
  (void)opath;

  if(close(fi->fh))
    return -errno;
  return 0;
}

static int convmvfs_getattr(const char *opath, struct stat *stbuf){
  string ipath = convmvfs.srcdir + out2in(opath);

  struct fuse_context *cont = fuse_get_context();
  int st = permission_walk_parent(ipath.c_str(), cont->uid, cont->gid,
                                  PERM_WALK_CHECK_EXEC);
  if(st)
    return st;

  if(lstat(ipath.c_str(), stbuf)){
    return -errno;
  }
  return 0;
}

static int convmvfs_opendir(const char *opath, struct fuse_file_info *fi){
  (void)fi;
  string ipath = convmvfs.srcdir + out2in(opath);

  /* permission check*/
  struct fuse_context *cont = fuse_get_context();
  int st = permission_walk(ipath.c_str(), cont->uid, cont->gid,
                           PERM_WALK_CHECK_READ);
  if(st)
    return st;

  return 0;
}

static int convmvfs_readdir(const char *opath, void *buf, fuse_fill_dir_t filler,
                         off_t offset, struct fuse_file_info *fi){
  (void)offset;
  (void)fi;
  string ipath = convmvfs.srcdir + out2in(opath);

  DIR * dir;
  if( (dir = opendir(ipath.c_str())) == NULL ){
    return -errno;
  }
  struct dirent *pdirent;
  pdirent = readdir(dir);
  while ( pdirent != NULL ) {
    filler(buf, in2out(pdirent->d_name).c_str(),
           NULL, 0);
    pdirent = readdir( dir );
  }
  closedir(dir);
  return 0;
}

static int convmvfs_mknod (const char *opath, mode_t mode, dev_t dev){
  string ipath = convmvfs.srcdir + out2in(opath);

  /* permission check*/
  struct fuse_context *cont = fuse_get_context();
  int st = permission_walk_parent(ipath.c_str(), cont->uid, cont->gid,
                                  PERM_WALK_CHECK_WRITE|PERM_WALK_CHECK_EXEC);
  if(st)
    return st;

  int rt = mknod(ipath.c_str(), mode, dev);
  if(rt)return -errno;
  if(euid == 0){
    chown(ipath.c_str(), cont->uid, cont->gid);
  }
  return 0;
}

static int convmvfs_mkdir (const char *opath, mode_t mode){
  string ipath = convmvfs.srcdir + out2in(opath);

  /* permission check*/
  struct fuse_context *cont = fuse_get_context();
  int st = permission_walk_parent(ipath.c_str(), cont->uid, cont->gid,
                                  PERM_WALK_CHECK_WRITE|PERM_WALK_CHECK_EXEC);
  if(st)
    return st;

  int rt = mkdir(ipath.c_str(), mode);
  if(rt)return -errno;
  if(euid == 0){
    chown(ipath.c_str(), cont->uid, cont->gid);
  }
  return 0;
}

static int convmvfs_readlink(const char *opath,
                             char *path, size_t path_len){
  string ipath = convmvfs.srcdir + out2in(opath);

  /* permission check*/
  struct fuse_context *cont = fuse_get_context();
  int st = permission_walk(ipath.c_str(), cont->uid, cont->gid,
                           PERM_WALK_CHECK_READ, 1);
  if(st)
    return st;

  st = readlink(ipath.c_str(), path, path_len-1);
  if(st == -1)
    return -errno;
  path[st] = '\0';
  ipath = in2out(path);
  strncpy(path, ipath.c_str(), min(path_len,ipath.size()+1));

  return 0;
}

static int convmvfs_unlink(const char *opath){
  string ipath = convmvfs.srcdir + out2in(opath);

  /* permission check*/
  struct fuse_context *cont = fuse_get_context();
  int st = permission_walk_parent(ipath.c_str(), cont->uid, cont->gid,
                                  PERM_WALK_CHECK_WRITE|PERM_WALK_CHECK_EXEC);
  if(st)
    return st;

  if(unlink(ipath.c_str()))
    return -errno;
  return 0;
}

static int convmvfs_rmdir(const char *opath){
  string ipath = convmvfs.srcdir + out2in(opath);

  /* permission check*/
  struct fuse_context *cont = fuse_get_context();
  int st = permission_walk_parent(ipath.c_str(), cont->uid, cont->gid,
                                  PERM_WALK_CHECK_WRITE|PERM_WALK_CHECK_EXEC);
  if(st)
    return st;

  if(rmdir(ipath.c_str()))
    return -errno;
  return 0;
}

static int convmvfs_symlink(const char *oldpath, const char *newpath){
  string inewpath = convmvfs.srcdir + out2in(newpath);

  /* permission check*/
  struct fuse_context *cont = fuse_get_context();
  int st = permission_walk_parent(inewpath.c_str(), cont->uid, cont->gid,
                                  PERM_WALK_CHECK_WRITE|PERM_WALK_CHECK_EXEC);
  if(st)
    return st;

  int rt = symlink(out2in(oldpath).c_str(), inewpath.c_str());
  if (rt) return -errno;

  if(euid == 0){
    lchown(inewpath.c_str(), cont->uid, cont->gid);
  }
  return 0;
}

static int convmvfs_rename(const char *oldpath, const char *newpath){
  string inewpath = convmvfs.srcdir + out2in(newpath);
  string ioldpath = convmvfs.srcdir + out2in(oldpath);

  /* permission check*/
  struct fuse_context *cont = fuse_get_context();
  int st = permission_walk_parent(inewpath.c_str(), cont->uid, cont->gid,
                                  PERM_WALK_CHECK_WRITE|PERM_WALK_CHECK_EXEC);
  if(st)
    return st;
  st = permission_walk_parent(ioldpath.c_str(), cont->uid, cont->gid,
                              PERM_WALK_CHECK_WRITE|PERM_WALK_CHECK_EXEC);
  if(st)
    return st;
  struct stat stbuf;
  if(stat(ioldpath.c_str(), &stbuf)){
    return -errno;
  }
  if(stbuf.st_mode & S_IFDIR){
    /* see rename(2), need write permission for renamed directory, because
     * needed to update the .. entry
     */
    st = permission_walk(ioldpath.c_str(), cont->uid, cont->gid,
                         PERM_WALK_CHECK_WRITE);
    if(st)
      return st;
  }

  if(rename(ioldpath.c_str(), inewpath.c_str()))
    return -errno;
  return 0;
}

static int convmvfs_link(const char *oldpath, const char *newpath){
  string inewpath = convmvfs.srcdir + out2in(newpath);
  string ioldpath = convmvfs.srcdir + out2in(oldpath);

  /* permission check*/
  struct fuse_context *cont = fuse_get_context();
  int st = permission_walk_parent(inewpath.c_str(), cont->uid, cont->gid,
                                  PERM_WALK_CHECK_WRITE|PERM_WALK_CHECK_EXEC);
  if(st)
    return st;
  st = permission_walk_parent(ioldpath.c_str(), cont->uid, cont->gid,
                              PERM_WALK_CHECK_WRITE|PERM_WALK_CHECK_EXEC);
  if(st)
    return st;

  if(link(ioldpath.c_str(), inewpath.c_str()))
    return -errno;
  return 0;
}

static int convmvfs_chmod(const char *opath, mode_t mode){
  string ipath = convmvfs.srcdir + out2in(opath);

  /* permission check*/
  struct fuse_context *cont = fuse_get_context();
  struct stat stbuf;
  if(stat(ipath.c_str(), &stbuf)){
    return -errno;
  }
  if((cont->uid != stbuf.st_uid) && (cont->uid != 0))
    return -EPERM;
  if(chmod(ipath.c_str(),mode))
    return -errno;
  return 0;
}

static int convmvfs_truncate(const char *opath, off_t length){
  string ipath = convmvfs.srcdir + out2in(opath);

  /* permission check*/
  struct fuse_context *cont = fuse_get_context();
  int st = permission_walk(ipath.c_str(), cont->uid, cont->gid,
                           PERM_WALK_CHECK_WRITE);
  if(st)
    return st;

  if(truncate(ipath.c_str(), length))
    return -errno;
  return 0;
}

static int convmvfs_utime(const char *opath, struct utimbuf *buf){
  string ipath = convmvfs.srcdir + out2in(opath);

  /* permission check*/
  struct fuse_context *cont = fuse_get_context();
  struct stat stbuf;
  if(stat(ipath.c_str(), &stbuf)){
    return -errno;
  }

  if(buf == NULL && permission_walk(ipath.c_str(), cont->uid, cont->gid,
                                    PERM_WALK_CHECK_WRITE)){
    return -errno;
  }
  if(buf != NULL && cont->uid != stbuf.st_uid)
    return -EPERM;

  if(utime(ipath.c_str(), buf))
    return -errno;
  return 0;
}

static int convmvfs_access(const char *opath, int mode){
  string ipath = convmvfs.srcdir + out2in(opath);

  if(mode & F_OK){
    struct stat stbuf;
    if(stat(ipath.c_str(),&stbuf)){
      return -errno;
    }
  }
  struct fuse_context *cont = fuse_get_context();
  return permission_walk(ipath.c_str(), cont->uid, cont->gid,
                         (mode & R_OK)?PERM_WALK_CHECK_READ:0 |
                         (mode & W_OK)?PERM_WALK_CHECK_WRITE:0 |
                         (mode & X_OK)?PERM_WALK_CHECK_EXEC:0
                         );
}

static int convmvfs_chown(const char *opath, uid_t uid_2set, gid_t gid_2set){
  string ipath = convmvfs.srcdir + out2in(opath);

  struct fuse_context *cont = fuse_get_context();
  /* FIX: grant access to chown if user is in target group */
  if(cont->uid == 0){
    if(chown(ipath.c_str(), uid_2set, gid_2set)){
      return -errno;
    }else{
      return 0;
    }
  }else{
    return -EPERM;
  }
}

static int convmvfs_statfs(const char *opath, struct statvfs *buf){
  string ipath = convmvfs.srcdir + out2in(opath);

  /* permission check*/
  struct fuse_context *cont = fuse_get_context();
  int st = permission_walk(ipath.c_str(), cont->uid, cont->gid,0);
  if(st)
    return st;

  if(statvfs(ipath.c_str(), buf)){
    return -errno;
  }

  return 0;
}

#if HAVE_ATTR_XATTR_H

static int convmvfs_listxattr(const char *opath, char *list, size_t listsize){
  string ipath = convmvfs.srcdir + out2in(opath);

  /* permission check*/
  struct fuse_context *cont = fuse_get_context();
  int st = permission_walk_parent(ipath.c_str(), cont->uid, cont->gid,
                                  PERM_WALK_CHECK_EXEC);
  if(st)
    return st;
  
  return llistxattr(ipath.c_str(), list, listsize);
}

static int convmvfs_removexattr(const char *opath, const char *xattr){
  string ipath = convmvfs.srcdir + out2in(opath);

  /* permission check*/
  struct fuse_context *cont = fuse_get_context();
  struct stat stbuf;
  if(stat(ipath.c_str(), &stbuf)){
    return -errno;
  }
  if((cont->uid != stbuf.st_uid) && (cont->uid != 0))
    return -EPERM;
  if(lremovexattr(ipath.c_str(), xattr))
    return -errno;
  return 0;
}

static int convmvfs_getxattr(const char *opath, const char *name, char *value, size_t valsize){
  string ipath = convmvfs.srcdir + out2in(opath);

  /* permission check*/
  struct fuse_context *cont = fuse_get_context();
  int st = permission_walk_parent(ipath.c_str(), cont->uid, cont->gid,
                                  PERM_WALK_CHECK_EXEC);
  if(st)
    return st;
  
  int res = lgetxattr(ipath.c_str(), name, value, valsize);
  if (res >= 0)
    return res;
  else
    return -errno;
}

static int convmvfs_setxattr(const char *opath, const char *name, const char *value, size_t valsize, int flags){
  string ipath = convmvfs.srcdir + out2in(opath);

  /* permission check*/
  struct fuse_context *cont = fuse_get_context();
  struct stat stbuf;
  if(stat(ipath.c_str(), &stbuf)){
    return -errno;
  }
  if((cont->uid != stbuf.st_uid) && (cont->uid != 0))
    return -EPERM;
  if(lsetxattr(ipath.c_str(), name, value, valsize, flags))
    return -errno;
  return 0;
}

#endif /* HAVE_ATTR_XATTR_H */

static void convmvfs_oper_init(){
  memset(&convmvfs_oper, 0, sizeof(convmvfs_oper));
  convmvfs_oper.getattr = convmvfs_getattr;
  convmvfs_oper.opendir = convmvfs_opendir;
  convmvfs_oper.readdir = convmvfs_readdir;
  convmvfs_oper.readlink = convmvfs_readlink;
  convmvfs_oper.mknod = convmvfs_mknod;
  convmvfs_oper.mkdir = convmvfs_mkdir;
  convmvfs_oper.unlink = convmvfs_unlink;
  convmvfs_oper.rmdir = convmvfs_rmdir;
  convmvfs_oper.symlink = convmvfs_symlink;
  convmvfs_oper.rename = convmvfs_rename;
  convmvfs_oper.link = convmvfs_link;
  convmvfs_oper.chmod = convmvfs_chmod;
  convmvfs_oper.chown = convmvfs_chown;
  convmvfs_oper.truncate = convmvfs_truncate;
  convmvfs_oper.utime = convmvfs_utime;
  convmvfs_oper.open = convmvfs_open;
  convmvfs_oper.read = convmvfs_read;
  convmvfs_oper.write = convmvfs_write;
  convmvfs_oper.release = convmvfs_release;
  convmvfs_oper.access = convmvfs_access;
  convmvfs_oper.statfs = convmvfs_statfs;
#if HAVE_ATTR_XATTR_H
  convmvfs_oper.listxattr = convmvfs_listxattr;
  convmvfs_oper.removexattr = convmvfs_removexattr;
  convmvfs_oper.getxattr = convmvfs_getxattr;
  convmvfs_oper.setxattr = convmvfs_setxattr;
#endif

  convmvfs_oper.init = convmvfs_init;
}


/*
 * life is here
 */
int main(int argc, char *argv[])
{
  int res;
  string srcdir;

  init_gvars();

  struct fuse_args args = FUSE_ARGS_INIT(argc, argv);  
  if (fuse_opt_parse(&args, &convmvfs, convmvfs_opts, convmvfs_opt_proc) == -1)
    exit(1);
  if(strlen(convmvfs.srcdir)){
    srcdir = convmvfs.srcdir;
    if(srcdir[srcdir.size()-1] == '/'){
      srcdir.erase(srcdir.size()-1,1);
    }
    size_t p;
    while((p = srcdir.find("//")) != string::npos){
      srcdir.erase(p,1);
    }
    convmvfs.srcdir = srcdir.c_str();
  }

  convmvfs_oper_init();

  fprintf(stderr,
          "srcdir=%s\n"
          "icharset=%s\n"
          "ocharset=%s\n",
          convmvfs.srcdir,
          convmvfs.icharset,
          convmvfs.ocharset);

  ic_out2in = iconv_open(convmvfs.icharset,convmvfs.ocharset);
  if( ic_out2in == (iconv_t)(-1) ){
    perror("iconv out2in");
    exit(1);
  }
  ic_in2out = iconv_open(convmvfs.ocharset,convmvfs.icharset);
  if( ic_in2out == (iconv_t)(-1) ){
    perror("iconv in2out");
    exit(1);
  }

  res = fuse_main(args.argc, args.argv, &convmvfs_oper);

  iconv_close(ic_out2in);
  iconv_close(ic_in2out);

  return res;
}
