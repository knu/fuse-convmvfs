/* (C) 2006 ZC Miao <hellwolf@seu.edu.cn>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define FUSE_USE_VERSION 25
#include <fuse.h>
#include <fuse_opt.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <iconv.h>

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <cassert>
#include <string>
#include <iostream>
#include <map>

using namespace std;

/*
 * global vars
 */
const char* CONVMVFS_DEFAULT_SRCDIR = "/"; /* root dir */
const char* CONVMVFS_DEFAULT_ICHARSET = "UTF-8";
const char* CONVMVFS_DEFAULT_OCHARSET = "UTF-8";

struct convmvfs {
  const char *srcdir;
  const char *icharset;
  const char *ocharset;
};
static struct convmvfs convmvfs;

int uid,gid;

void init_gvars(){

  convmvfs.srcdir = CONVMVFS_DEFAULT_SRCDIR;
  convmvfs.icharset = CONVMVFS_DEFAULT_ICHARSET;
  convmvfs.ocharset =  CONVMVFS_DEFAULT_OCHARSET;

  uid = geteuid();
  gid = getgid();
}

static struct fuse_operations convmvfs_oper;
struct fdmap_node{
  fdmap_node(){}
  fdmap_node(int f):fd(f),use(1){}
  int fd;
  int use;
};
typedef map<string, fdmap_node> FDMAP;
FDMAP fd_map;

iconv_t ic_out2in,ic_in2out;




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
    size_t niconv = iconv(ic,
                   &inbuf,&ibleft,
                   &outbuf,&obleft);
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

int getfd(const char *opath, struct fuse_file_info *fi,
          int upuse = 1){
  (void)fi;
  FDMAP::iterator fditer;
  if( (fditer = fd_map.find(opath)) == fd_map.end() ){
    return -1;
  }else{
    if(upuse){(fditer->second).use++;}
    return (fditer->second).fd;
  }
}

int setfd(const char *opath, struct fuse_file_info *fi,
          int fd){
  (void)fi;
  assert( fd_map.find(opath) == fd_map.end() );
  fd_map[opath] = fdmap_node(fd);
  return 0;
}

int putfd(const char *opath, struct fuse_file_info *fi){
  (void)fi;
  FDMAP::iterator fditer;
  if( (fditer = fd_map.find(opath)) == fd_map.end() ){
    return -ENOENT;
  }else{
    (fditer->second).use--;
    if( (fditer->second).use == 0 ){
      int fd = (fditer->second).fd;
      cerr << "     erase " << fd << "\n"; 
      fd_map.erase(fditer);
      if(close(fd)){
        return -errno;
      }
    }
  }
  return 0;
}



/*
 * opers
 */
static int convmvfs_open(const char *opath, struct fuse_file_info *fi){
  cerr << __FUNCTION__ << " " << opath;
  string ipath = convmvfs.srcdir + out2in(opath);
  cerr << " -> " << ipath << "\n";
 
  if( getfd(opath,fi) == -1 ){
    /* new open */
    int fd;
    if( (fd = open(ipath.c_str(),fi->flags)) == -1 ){
      return -errno;
    }
    cerr << "new open " << fd << "\n";
    if( setfd(opath,fi,fd) ){
      return -ENODATA;
    }
  }
  return 0;
}

static int convmvfs_release(const char *opath, struct fuse_file_info *fi){
  cerr << __FUNCTION__ << " " << opath << "\n";

  return putfd(opath,fi);
}

static int convmvfs_getattr(const char *opath, struct stat *stbuf){
  cerr << __FUNCTION__ << " " << opath;
  string ipath = convmvfs.srcdir + out2in(opath);
  cerr << " -> " << ipath << "\n";

  if(stat(ipath.c_str(), stbuf)){
    return -errno;
  }
  return 0;
}

static int convmvfs_read(const char *opath, char *buf,
                         size_t size, off_t offset,
                         struct fuse_file_info *fi){
  cerr << __FUNCTION__ << " " << opath << "\n";
  int fd = getfd(opath, fi, 0);
  if(fd == -1){
    return -ENOENT;
  }else{
    lseek(fd, offset, SEEK_SET);
    return read(fd, buf, size);
  }
}

static int convmvfs_readdir(const char *opath, void *buf, fuse_fill_dir_t filler,
                         off_t offset, struct fuse_file_info *fi){
  (void)fi;
  cerr << __FUNCTION__ << " " << opath;
  string ipath = convmvfs.srcdir + out2in(opath);
  cerr << " -> " << ipath << "\n";

  DIR * dir;
  if( (dir = opendir(ipath.c_str())) == NULL ){
    return -errno;
  }
  seekdir(dir, offset);
  struct dirent *pdirent;
  pdirent = readdir(dir);
  while ( pdirent != NULL ) {
    filler(buf, in2out(pdirent->d_name).c_str(),
           NULL, pdirent->d_off);
    pdirent = readdir( dir );
  }
  closedir(dir);
  return 0;
}

static void convmvfs_oper_init(){
  memset(&convmvfs_oper, 0, sizeof(convmvfs_oper));
  convmvfs_oper.getattr = convmvfs_getattr;
  convmvfs_oper.open = convmvfs_open;
  convmvfs_oper.read = convmvfs_read;
  convmvfs_oper.readdir = convmvfs_readdir;
  convmvfs_oper.release = convmvfs_release;
}


/*
 * life is here
 */
int main(int argc, char *argv[])
{
  int res;

  init_gvars();

  struct fuse_args args = FUSE_ARGS_INIT(argc, argv);  
  if (fuse_opt_parse(&args, &convmvfs, convmvfs_opts, convmvfs_opt_proc) == -1)
    exit(1);
  if(convmvfs.srcdir == NULL){  }

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
