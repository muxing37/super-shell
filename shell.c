#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/stat.h>
#include <errno.h>
#include <termios.h>
#include <pthread.h>
#include <math.h>
#include <readline/readline.h>
#include <readline/history.h>
#define MAX_PATH 1024

int running=0;
int back_count=0;

struct command {
    char **argv;
    size_t argv_alloc;
    int argc;
    char *input_file;
    char *output_file;
    int append;
};

struct oldcwd {
    char path[MAX_PATH];
    int set;
};

typedef enum{
    WORD,
    PIPE, // |
    REDIR_IN, // <
    REDIR_OUT, // >
    REDIR_APPEND, // >>
    BACK // &
} TokenType;

struct Token {
    char *word;
    TokenType type;
    size_t alloc_word;
};

struct Back {
    int num;
    pid_t pid[64];
    char cmd[512];
    int finished;
    int pid_count;
    struct Back *next;
};

struct Back *head;

void Back_add(int token_num,struct Token *token) {
    int i;
    int l=0;
    struct Back *p=head;
    if(p->next==NULL) {
        p->next=calloc(1,sizeof(struct Back));
        for(i=0;i<token_num;i++) {
            if(token[i].type==WORD) {
                strcat(p->next->cmd,token[i].word);
                l=l+strlen(token[i].word)+1;
                if(i!=token_num-1) {
                    p->next->cmd[l-1]=' ';
                    p->next->cmd[l]='\0';
                }
            } else {
                if(token[i].type==REDIR_APPEND) {
                    strcat(p->next->cmd,">> ");
                    l=l+3;
                } else if(token[i].type==PIPE) {
                    strcat(p->next->cmd,"| ");
                    l=l+2;
                } else if(token[i].type==REDIR_IN) {
                    strcat(p->next->cmd,"< ");
                    l=l+2;
                } else if(token[i].type==REDIR_OUT) {
                    strcat(p->next->cmd,"> ");
                    l=l+2;
                }
            }
        }
        p->next->num=++back_count;
        return;
    }
    while(p->next!=NULL) {
        p=p->next;
        if(p->next==NULL) {
            p->next=calloc(1,sizeof(struct Back));
            for(i=0;i<token_num;i++) {
                if(token[i].type==WORD) {
                    strcat(p->next->cmd,token[i].word);
                    l=l+strlen(token[i].word)+1;
                    if(i!=token_num-1) {
                        p->next->cmd[l-1]=' ';
                        p->next->cmd[l]='\0';
                    }
                } else {
                    if(token[i].type==REDIR_APPEND) {
                        strcat(p->next->cmd,">> ");
                        l=l+3;
                    } else if(token[i].type==PIPE) {
                        strcat(p->next->cmd,"| ");
                        l=l+2;
                    } else if(token[i].type==REDIR_IN) {
                        strcat(p->next->cmd,"< ");
                        l=l+2;
                    } else if(token[i].type==REDIR_OUT) {
                        strcat(p->next->cmd,"> ");
                        l=l+2;
                    }
                }
            }
            p->next->num=++back_count;
            break;
        }
    }
}

void Back_delete() {
    struct Back *prev=head;
    struct Back *cur=head->next;
    struct Back *temp;
    while(cur!=NULL) {
        temp=cur->next;
        if(cur->pid_count==cur->finished){
            prev->next=cur->next;
            printf("[%d] 已完成    %s\n",cur->num,cur->cmd);
            free(cur);
            cur=temp;
        } else {
            prev=cur;
            cur=temp;
        }
    }
}

void pid_delete(pid_t pid) {
    int i;
    int found=0;
    struct Back *p=head->next;
    while(1) {
        if(p==NULL) return;
        for(i=0;i<p->pid_count;i++) {
            if(pid==p->pid[i]) {
                p->finished++;
                found=1;
                break;
            }
        }
        p=p->next;
    }
}

void pid_add(pid_t *pid,int cmd_num) {
    int i;
    struct Back *p=head->next;
    while(p->next!=NULL) {
        p=p->next;
    }
    for(i=0;i<cmd_num;i++) {
        p->pid[i]=pid[i];
    }
    p->pid_count=cmd_num;
    p->finished=0;
}

void* grow_alloc(size_t c,size_t *a,size_t size,void *g) {
    if(c>*a-4 || *a==0) {
        if(*a==0) {
            *a=8;
            g=calloc(*a,size);
        } else {
            g=realloc(g,size*(*a)*2);
            memset(g+(*a)*size,0,(*a)*size);
            *a=(*a)*2;
        }
    }
    return g;
}

int cd(char *str) {
    static struct oldcwd last_path={0};
    char t[MAX_PATH]={0};
    getcwd(t,sizeof(t));
    if(strlen(str)==1 && str[0]=='-') {
        if(last_path.set==0) {
            printf("cd: OLDPWD 未设定\n");
            return -1;
        } else {
            printf("%s\n",last_path.path);
            chdir(last_path.path);
            strcpy(last_path.path,t);
            last_path.set=1;
            return 0;
        }
    }
    if(str[0]=='~') {
        char temp[MAX_PATH]={0};
        strcpy(temp,getenv("HOME"));
        strcat(temp,str+1);
        if(chdir(temp)==-1) {
            perror("cd: ");
        }
        strcpy(last_path.path,t);
        last_path.set=1;
        return 0;
    }
    if(chdir(str)==-1) {
        perror("cd: ");
    }
    strcpy(last_path.path,t);
    last_path.set=1;
    return 0;
}

void handle_SIGINT() {
    struct timespec stop;
    stop.tv_sec=0;
    stop.tv_nsec=2000000;
    nanosleep(&stop,NULL);
    printf("\n");
    if(running==0) {
        rl_replace_line("",0);
        rl_on_new_line();
        rl_redisplay();
    }
}

void handle_SIGTSTP() {
    if(running==1) {
        printf("\n");
        // rl_replace_line("",0);
        // rl_on_new_line();
        // rl_redisplay();
    }
}

void handle_SIGCHLD() {
    int status;
    pid_t pid;
    pid=waitpid(-1,&status,WNOHANG);
    pid_delete(pid);
}

void handle_signal(){
    signal(SIGINT,handle_SIGINT);
    signal(SIGTSTP,handle_SIGTSTP);
    signal(SIGCHLD,handle_SIGCHLD);
}

void restore_signal() {
    signal(SIGINT,SIG_DFL);
    signal(SIGTSTP,SIG_DFL);
}

void hsv_to_rgb(float h, float s, float v, int *r, int *g, int *b) {
    float c = v * s;
    float x = c * (1 - fabs(fmod(h / 60.0, 2) - 1));
    float m = v - c;
    float r1, g1, b1;

    if(h >= 0 && h < 60)      { r1 = c; g1 = x; b1 = 0; }
    else if(h < 120)          { r1 = x; g1 = c; b1 = 0; }
    else if(h < 180)          { r1 = 0; g1 = c; b1 = x; }
    else if(h < 240)          { r1 = 0; g1 = x; b1 = c; }
    else if(h < 300)          { r1 = x; g1 = 0; b1 = c; }
    else                      { r1 = c; g1 = 0; b1 = x; }

    *r = (int)((r1 + m) * 255);
    *g = (int)((g1 + m) * 255);
    *b = (int)((b1 + m) * 255);
}

void get_colorful(char *prompt,const char *s) {
    int i;
    int len=strlen(s);
    int n=0;
    for(i=0;i<len;i++) {
        double h=(double)i/len*360;
        int r,g,b;
        hsv_to_rgb(h,1.0,1.0,&r,&g,&b);
        int l=snprintf(prompt+n,MAX_PATH+512-n,"\001\033[38;2;%d;%d;%dm\002%c\001\033[0m\002",r,g,b,s[i]);
        n=n+l;
    }
}

void get_prompt(char *prompt) {
    int i;
    get_colorful(prompt,"zht-super-shell");

    char now_path[MAX_PATH];
    getcwd(now_path,sizeof(now_path));
    char *home=getenv("HOME");
    if(strncmp(now_path,home,strlen(home))==0) {
        int len=strlen(home);
        char temp[MAX_PATH]="~";
        for(i=0;i<strlen(now_path)-len;i++) {
            temp[i+1]=now_path[len+i];
        }
        temp[i+1]=0;
        strcpy(now_path,temp);
    }
    char temp[MAX_PATH+64];
    snprintf(temp,sizeof(temp),":\033[1;34m%s\033[0m$ ",now_path);
    strncat(prompt,temp,sizeof(prompt)-strlen(prompt)-1);
}

void free_token(int token_num,struct Token *token) {
    int i;
    for(i=0;i<token_num;i++) {
        if(token[i].word!=NULL) free(token[i].word);
    }
    free(token);
}

void free_cmd(int cmd_num,struct command *cmd) {
    int i,j;
    for(i=0;i<cmd_num;i++) {
        for(j=0;j<cmd[i].argc;j++) {
            free(cmd[i].argv[j]);
        }
        free(cmd[i].argv);
        if(cmd[i].input_file!=NULL) free(cmd[i].input_file);
        if(cmd[i].output_file!=NULL) free(cmd[i].output_file);
    }
    free(cmd);
}

void free_back() {
    struct Back *p=head->next;
    free(head);
    while(p!=NULL) {
        struct Back *t=p->next;
        free(p);
        p=t;
    }
}

struct Token *gettoken(char *input,int *token_num) {
    struct Token *token=NULL;
    size_t token_alloc=0;
    int l=strlen(input);
    int i,j;
    int k=0;
    int yinhao=0;
    int ifspace=0;
    int notword=0;
    for(i=0,j=0;i<l;i++) {
        ifspace=0;
        notword=0;
        token=grow_alloc(j,&token_alloc,sizeof(struct Token),token);
        if(input[i]=='"') {
            if(yinhao==0) {
                yinhao=1;
                continue;
            }
            if(yinhao==1) {
                yinhao=0;
                continue;
            }
        }
        if(input[i]=='>' && input[i+1]=='>' && yinhao==0) {
            if(token[j].word!=NULL) {
                j++;
            }
            token[j++].type=REDIR_APPEND;
            i++;
            k=0;
            notword=1;
            continue;
        } else if(input[i]=='|' && yinhao==0) {
            if(token[j].word!=NULL) {
                j++;
            }
            token[j++].type=PIPE;
            k=0;
            notword=1;
            continue;
        } else if(input[i]=='<' && yinhao==0) {
            if(token[j].word!=NULL) {
                j++;
            }
            token[j++].type=REDIR_IN;
            k=0;
            notword=1;
            continue;
        } else if(input[i]=='>' && yinhao==0) {
            if(token[j].word!=NULL) {
                j++;
            }
            token[j++].type=REDIR_OUT;
            k=0;
            notword=1;
            continue;
        } else if(input[i]=='&' && yinhao==0) {
            if(token[j].word!=NULL) {
                j++;
            }
            token[j++].type=BACK;
            k=0;
            ifspace=1;
            continue;
        }

        if(input[i]==' ' && yinhao==0) {
            k=0;
            if(token[j].word!=NULL) {
                j++;
            }
            ifspace=1;
            continue;
        }
        token[j].type=WORD;
        token[j].word=grow_alloc(k,&token[j].alloc_word,sizeof(char),token[j].word);
        token[j].word[k]=input[i];
        k++;
    }
    if(ifspace) {
        *token_num=j;
    } else if(notword) {
        *token_num=0;
        free_token(--j,token);
        printf("有语法错误\n");
    } else{
        *token_num=++j;
    }
    //free(input);
    return token;
}

struct command *getcmd(struct Token *token,int token_num,int *cmd_num) {
    int i,j,k;
    struct command *cmd=NULL;
    size_t cmd_alloc=0;
    int isls=0;
    for(i=0,j=0;i<token_num;i++) {
        cmd=grow_alloc(j,&cmd_alloc,sizeof(struct command),cmd);
        if(token[i].type==PIPE) {
            cmd[j].argv[cmd[j].argc]=NULL;
            j++;
            isls=0;
        } else if(token[i].type==REDIR_IN) {
            int l=strlen(token[++i].word);
            cmd[j].input_file=calloc(l+1,sizeof(char));
            for(k=0;k<l;k++) {
                cmd[j].input_file[k]=token[i].word[k];
            }
        } else if(token[i].type==REDIR_OUT || token[i].type==REDIR_APPEND) {
            int l=strlen(token[++i].word);
            cmd[j].output_file=calloc(l+1,sizeof(char));
            for(k=0;k<l;k++) {
                cmd[j].output_file[k]=token[i].word[k];
            }
            if(token[i-1].type==REDIR_APPEND) cmd[j].append=1;
        } else if(token[i].type==WORD) {
            if(cmd[j].argv==NULL) cmd[j].argv_alloc=0;
            cmd[j].argv=grow_alloc(cmd[j].argc,&cmd[j].argv_alloc,sizeof(char*),cmd[j].argv);
            k=0;
            int l=strlen(token[i].word);
            cmd[j].argv[cmd[j].argc]=calloc(l+1,sizeof(char));
            for(k=0;k<l;k++) {
                cmd[j].argv[cmd[j].argc][k]=token[i].word[k];
            }
            cmd[j].argc++;
        }
        if(cmd[j].argv!=NULL && (!strcmp("ls",cmd[j].argv[0]) || !strcmp("grep",cmd[j].argv[0])) && isls==0) {
            cmd[j].argv[cmd[j].argc]=calloc(16,sizeof(char));
            strcpy(cmd[j].argv[cmd[j].argc++],"--color=auto");
            isls=1;
        }
    }
    *cmd_num=++j;
    return cmd;
}

int run_cmd(struct command *cmd,int cmd_num,int ifback) {
    running=1;
    int i,j;
    int status;
    int pipes[cmd_num][2];
    if(cmd_num>1) {
        for(j=0;j<cmd_num-1;j++) {
            pipe(pipes[j]);
        }
    }
    pid_t pid[cmd_num];
    for(i=0;i<cmd_num;i++) {
        pid[i]=fork();
        
        if(pid[i]==0) {
            restore_signal();
            if(cmd_num>1) {
                if(i>0) dup2(pipes[i-1][0],STDIN_FILENO);
                if(i<cmd_num-1) dup2(pipes[i][1],STDOUT_FILENO);
            }
            for(j=0;j<cmd_num-1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            if(cmd[i].input_file!=NULL) {
                int fd=open(cmd[i].input_file,O_RDONLY);
                if(fd==-1) {
                    perror("open");
                    exit(1);
                }
                dup2(fd,STDIN_FILENO);
                close(fd);
            }
            if(cmd[i].output_file!=NULL && cmd[i].append==0) {
                int fd=open(cmd[i].output_file,O_WRONLY | O_CREAT | O_TRUNC,0644);
                if(fd==-1) {
                    perror("open");
                    exit(1);
                }
                dup2(fd,STDOUT_FILENO);
                close(fd);
            }
            if(cmd[i].output_file!=NULL && cmd[i].append==1) {
                int fd=open(cmd[i].output_file,O_WRONLY | O_CREAT | O_APPEND,0644);
                if(fd==-1) {
                    perror("open");
                    exit(1);
                }
                dup2(fd,STDOUT_FILENO);
                close(fd);
            }
            execvp(cmd[i].argv[0],cmd[i].argv);
            perror("execvp");
            exit(1);
        }
    }
    for(i=0;i<cmd_num-1;i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
    if(!ifback) {
        for(i=0;i<cmd_num;i++) {
            waitpid(-1,&status,WUNTRACED);
        }
    }
    if(ifback) {
        printf("[%d] %d\n",back_count,(int)pid[cmd_num-1]);
        pid_add(pid,cmd_num);
    }
    running=0;
    return 0;
}

int main() {
    handle_signal();
    chdir(getenv("HOME"));
    head=calloc(1,sizeof(struct Back));
    while(1) {
        int ifback=0;
        int i,j,k;
        char prompt[MAX_PATH+512]={0};
        get_prompt(prompt);
        char *input=NULL;

        input=readline(prompt);

        // printf("%s",prompt);
        // input=calloc(1024,sizeof(char));
        // fgets(input,1024,stdin);
        // printf("%s",input);
        // input[strlen(input)-1]=0;

        if(back_count!=0) Back_delete();
        if(input==NULL) {
            free(input);
            continue;
        }
        if(strlen(input)==0) {
            free(input);
            continue;
        }
        add_history(input);
        int token_num=0;
        struct Token *token=gettoken(input,&token_num);
        free(input);
        if(token_num==0) {
            continue;
        }
        if(token_num==1 && !strcmp("exit",token[0].word)) {
            signal(SIGCHLD,SIG_IGN);
            free_token(token_num,token);
            free_back();
            rl_clear_history();
            exit(0);
        }

        for(i=0;i<token_num;i++) {
            if((token[i].type==BACK && i<token_num-1) || token[0].type==BACK) {
                printf("未预期的记号 \"&\" 附近有语法错误\n");
                free_token(token_num,token);
                ifback=2;
                break;
            }
            if(token[i].type==BACK && i==token_num-1) {
                ifback=1;
                token_num--;
            }
        }
        if(token==NULL || ifback==2) {
            ifback=0;
            continue;
        }
        if(ifback==1) Back_add(token_num,token);

        int cmd_num=0;
        struct command *cmd=getcmd(token,token_num,&cmd_num);
        free_token(token_num,token);
        if(cmd_num==1 && !strcmp("cd",cmd[0].argv[0])) {
            if(cmd[0].argc>2) {
                printf("cd: 参数太多\n");
                free_cmd(cmd_num,cmd);
                continue;
            }
            cd(cmd[0].argv[1]);
            free_cmd(cmd_num,cmd);
            continue;
        }
        run_cmd(cmd,cmd_num,ifback);
        free_cmd(cmd_num,cmd);
    }
    return 0;
}