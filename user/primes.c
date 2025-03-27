#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
int main(){                                                                     
        int nums[35]={0};
        for(int i=0;i<=33;i++)
                nums[i]=i+2;
        nums[34]=0;
	int p[2];
        while(nums[0]<=35 && nums[0]){
                printf("prime %d\n",nums[0]);
                pipe(p);
                if(fork()==0){
                        close(p[1]);
                        int i=0;
                        while(read(p[0],nums+i,sizeof(int))>0)
				i++;
                        nums[i]=0;
                        close(p[0]);
                }
                else{
                        close(p[0]);
                        for(int i=1;nums[i]!=0;i++){
                                if(nums[i]%nums[0]!=0)
                                        write(p[1],nums+i,sizeof(int));
                        }
			close(p[1]);
			wait(0);
			exit(0);
                }
        } 
       	exit(0);
}                             
