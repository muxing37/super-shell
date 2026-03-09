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

struct Token *gettoken(char *input,int *token_num) {
    struct Token *token=NULL;
    size_t token_alloc=0;

    int l=strlen(input);
    int i,j;
    int k=0;
    int yinhao=0;
    for(i=0,j=0;i<l;i++) {
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
            continue;
        } else if(input[i]=='|' && yinhao==0) {
            if(token[j].word!=NULL) {
                j++;
            }
            token[j++].type=PIPE;
            k=0;
            continue;
        } else if(input[i]=='<' && yinhao==0) {
            if(token[j].word!=NULL) {
                j++;
            }
            token[j++].type=REDIR_IN;
            k=0;
            continue;
        } else if(input[i]=='>' && yinhao==0) {
            if(token[j].word!=NULL) {
                j++;
            }
            token[j++].type=REDIR_OUT;
            k=0;
            continue;
        } else if(input[i]=='&' && yinhao==0) {
            if(token[j].word!=NULL) {
                j++;
            }
            token[j++].type=BACK;
            k=0;
            continue;
        }

        if(input[i]==' ' && yinhao==0) {
            k=0;
            if(token[j].word!=NULL) {
                j++;
            }
            continue;
        }
        token[j].type=WORD;
        token[j].word=grow_alloc(k,&token[j].alloc_word,sizeof(char),token[j].word);
        token[j].word[k]=input[i];
        k++;
    }
    free(input);
    *token_num=++j;
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

int run_cmd(struct command *cmd,int cmd_num) {
    int i,j;
    int pipes[cmd_num][2];
    if(cmd_num>1) {
        for(j=0;j<cmd_num-1;j++) {
            pipe(pipes[j]);
        }
    }
    for(i=0;i<cmd_num;i++) {
        pid_t pid=fork();

        if(pid==0) {
            if(cmd_num>1) {
                if(i>0) dup2(pipes[i-1][0],STDIN_FILENO);
                if(i<cmd_num-1) dup2(pipes[i][1],STDOUT_FILENO);
            }
            for(j = 0; j < cmd_num - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            if(cmd[i].input_file!=NULL) {
                int fd=open(cmd[i].input_file,O_RDONLY);
                if(fd==-1) {
                    perror("open");
                    exit(1);
                }
                dup2(fd, STDIN_FILENO);
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
    for(i=0;i<cmd_num;i++) {
        wait(NULL);
    }
    return 0;
}

int cd(char *str) {
    static struct oldcwd last_path={0};
    char t[MAX_PATH]={0};
    getcwd(t,sizeof(t));
    if(strlen(str)==1) {
        if(str[0]=='-') {
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
    for(i=0;i<len;i++){
        double h=(double)i/len*360;
        int r,g,b;
        hsv_to_rgb(h,1.0,1.0,&r,&g,&b);
        int l=snprintf(prompt+n,MAX_PATH+512-n,"\001\033[38;2;%d;%d;%dm\002%c\001\033[0m\002",r,g,b,s[i]);
        n=n+l;
    }
}

int main() {
    chdir(getenv("HOME"));
    while(1) {
        int i,j,k;
        char prompt[MAX_PATH+512]={0};
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

        char *input=NULL;
        input=readline(prompt);

        if(strlen(input)==0) continue;
        int token_num;
        struct Token *token=gettoken(input,&token_num);
        if(token_num==1 && !strcmp("exit",token[0].word)) {
            free(token[0].word);
            free(token);
            exit(0);
        }

        int cmd_num;
        struct command *cmd=getcmd(token,token_num,&cmd_num);

        if(cmd_num==1 && !strcmp("cd",cmd[0].argv[0])) {
            if(cmd[0].argc>2) {
                printf("cd: 参数太多\n");
                continue;
            }
            cd(cmd[0].argv[1]);
            continue;
        }

        run_cmd(cmd,cmd_num);

        // for(i=0;i<token_num;i++) {
        //     if(token[i].word!=NULL) printf("%s\n",token[i].word);
        //     else printf("NULL\n");
        //     printf("%d\n",token[i].type);
        // }

        // for(i=0;i<cmd_num;i++) {
        //     printf("cmd%d\n",i+1);
        //     for(k=0;k<cmd[i].argc;k++) {
        //         printf("%s\n",cmd[i].argv[k]);
        //     }
        //     if(cmd[i].input_file!=NULL) printf("in %s\n",cmd[i].input_file);
        //     if(cmd[i].output_file!=NULL) printf("out %s\n",cmd[i].output_file);
        //     printf("%d\n",cmd[i].append);
        //     printf("\n");
        // }
        for(i=0;i<token_num;i++) {
            if(token[i].word!=NULL) free(token[i].word);
        }
        free(token);

        for(i=0;i<cmd_num;i++) {
            for(k=0;k<cmd[i].argc;k++) {
                free(cmd[i].argv[k]);
            }
            free(cmd[i].argv);
            if(cmd[i].input_file!=NULL) free(cmd[i].input_file);
            if(cmd[i].output_file!=NULL) free(cmd[i].output_file);
        }
        free(cmd);
    }
    return 0;
}