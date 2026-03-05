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

struct command {
    char **argv;
    size_t argv_alloc;
    int argc;
    char *input_file;
    char *output_file;
    int append;
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
    if(c>*a-2 || *a==0) {
        if(*a==0) {
            *a=4;
            g=calloc(*a,size);
        } else {
            g=realloc(g,size*(*a)*2);
            memset(g+(*a)*size,0,(*a)*size);
            *a=(*a)*2;
        }
    }
    return g;
}

int main() {
    while(1) {
        printf("zht-super-shell:");
        char now_path[1024];

        getcwd(now_path,sizeof(now_path));
        char *home=getenv("HOME");
        if(strncmp(now_path,home,strlen(home))==0) printf("~%s$ ",now_path+strlen(home));
        else printf("%s$ ",now_path);
        struct Token *token=NULL;
        size_t token_alloc=0;

        char *input=NULL;
        size_t len=0;
        getline(&input,&len,stdin);
        int l=strlen(input);
        input[--l]='\0';
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
                token[j++].type=REDIR_APPEND;
                i++;
                k=0;
                continue;
            } else if(input[i]=='|' && yinhao==0) {
                token[j++].type=PIPE;
                // i++;
                k=0;
                continue;
            } else if(input[i]=='<' && yinhao==0) {
                token[j++].type=REDIR_IN;
                // i++;
                k=0;
                continue;
            } else if(input[i]=='>' && yinhao==0) {
                token[j++].type=REDIR_OUT;
                // i++;
                k=0;
                continue;
            } else if(input[i]=='&' && yinhao==0) {
                token[j++].type=BACK;
                // i++;
                k=0;
                continue;
            }

            if(input[i]==' ' && yinhao==0) {
                //token[j].word[k]=0;
                k=0;
                if(token[j].word!=NULL) {
                    // token[j].type=WORD;
                    j++;
                }

                continue;
            }
            token[j].type=WORD;
            token[j].word=grow_alloc(k,&token[j].alloc_word,sizeof(char),token[j].word);
            token[j].word[k]=input[i];
            k++;
        }

        int token_num=++j;
        free(input);
        struct command *cmd;
        size_t cmd_alloc=0;
        cmd=grow_alloc(j,&cmd_alloc,sizeof(struct command),cmd);
        for(i=0,j=0;i<token_num;i++) {
            cmd=grow_alloc(j,&cmd_alloc,sizeof(struct command),cmd);
            if(token[i].type==PIPE) {
                j++;
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
                cmd[j].append=1;
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
        }

        for(i=0;i<token_num;i++) {
            if(token[i].word!=NULL) printf("%s\n",token[i].word);
            else printf("NULL\n");
            printf("%d\n",token[i].type);
        }

        for(i=0;i<=j;i++) {
            printf("cmd%d\n",i+1);
            for(k=0;k<cmd[i].argc;k++) {
                printf("%s\n",cmd[i].argv[k]);
            }
            if(cmd[i].input_file!=NULL) printf("in %s\n",cmd[i].input_file);
            if(cmd[i].output_file!=NULL) printf("out %s\n",cmd[i].output_file);
            printf("\n");
        }

        
        // printf("%s\n",input);
    }
    return 0;
}