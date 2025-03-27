#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
void find(char *cur_dir,char *filename){
	int fd;
	if((fd=open(cur_dir,0))<0)
		printf("cannot open the dir%s\n",cur_dir);
	struct stat st;
	struct dirent de;
	char *p;
	p=cur_dir+strlen(cur_dir);
	*p++='/';//point at tail of dir string
	while(read(fd,&de,sizeof(de))==sizeof(de)){//read file info one by one
		if(strcmp(de.name,"")==0)
			break;
		memmove(p,de.name,sizeof(de.name));//make a whole filepath
		stat(cur_dir,&st);//get fileinfo
		if(strcmp(de.name,".")==0 || strcmp(de.name,"..")==0)
			continue;
		if(st.type==T_DIR)
			find(cur_dir,filename);
		else{
			if(strcmp(de.name,filename)==0)
				printf("%s\n",cur_dir);
		}
	}
}
int main(int argc,char* argv[]){
	char dir_path[DIRSIZ];
	char target[DIRSIZ];
	if(argc==1)
		printf("please enter a filename you want to find!\n");
	if(argc==2){
		memmove(target,argv[1],strlen(argv[1]));
		dir_path[0]='.';
	}
	if(argc==3){
		if(argv[1][0]!='.'){
			dir_path[0]='.';
			dir_path[1]='/';
			memmove(dir_path+2,argv[1],strlen(argv[1]));
			memmove(target,argv[2],strlen(argv[2]));
		}
		else{
			memmove(dir_path,argv[1],strlen(argv[1]));
			memmove(target,argv[2],strlen(argv[2]));
		}
	}
	find(dir_path,target);
	exit(0);
}
