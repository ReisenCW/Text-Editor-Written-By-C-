/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <termios.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <string.h>
#include <time.h>

/*** defines ***/

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 4
#define KILO_QUIT_TIMES 3

#define CTRL_KEY(key) ((key) & 0x1f) // 0x1f是ctrl

enum editorKey{
    BACKSPACE = 127,
    ARROW_LEFT = 1000, //使用大int,防止与键位冲突
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

enum editorHighlight{
    HL_NORMAL = 0,
    HL_COMMENT,
    HL_MLCOMMENT,
    HL_KEYWORD1,
    HL_KEYWORD2,
    HL_STRING,
    HL_NUMBER,
    HL_MATCH
};

#define HL_HIGHLIGHT_NUMBERS (1 << 0)
#define HL_HIGHLIGHT_STRINGS (1 << 1)
/*** data ***/

struct editorSyntax{
    char *filetype; // 文件类型(如.c)
    char **filematch;
    char **keywords;
    char *singleline_comment_start; // 单行注释所用符号
    char *multiline_comment_start;
    char *multiline_comment_end;
    int flags;
};

typedef struct {
    int idx; // 自己在数组中的idx
    int size; // 大小
    int rsize; //将tab转化为空格后的实际大小
    char *chars; // 字符串
    char *render; // 将tab转化为空格,控制字符的转化(如ctrl + A -> ^A)
    unsigned char *hl; // array, 存储元素类型为enum editorHighlight  
    int hl_open_comment; // 是否有未闭合的多行注释
} erow;

struct editorConfig
{
    int cx, cy; // 光标在文件中的x,y
    int rx; // 如果没有yab,和cx一样,如果有tab，比cx大
    int rowoff; // 页面上下滚动到了哪一行
    int coloff; // 页面左右滚动到了哪一页
	int screen_rows; // 屏幕行数
	int screen_cols; // 屏幕列数
    int numrows; // 读取的行数
    erow *row; // 读取的文本
    int dirty; // 文本是否有修改
    char *filename; // 文件名
    char statusmsg[80];
    time_t statusmsg_time;
    struct editorSyntax *syntax;
	struct termios orig_termios;
};

struct editorConfig E;

/*** filetypes ***/

char *C_HL_extensions[] = { ".c", ".h", ".cpp", ".hpp", NULL };

char *C_HL_keywords[] = { // 末尾有|的属于keywords2
    "switch", "if", "while", "for", "break", "continue", "return", "else",
    "struct", "union", "typedef", "static", "enum", "class", "case",
    "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
    "void|", NULL
};

struct editorSyntax HLDB[] = { // highlight database
    {
        "c",
        C_HL_extensions,
        C_HL_keywords,
        "//", "/*", "*/",
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
    },
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0])) // HLDB 的长度

/*** prototypes ***/
void EditorSetStatusMessage(const char *fmt, ...);
void EditorRefreshScreen();
char *EditorPrompt(char *prompt, void (*callback)(char *, int));


/*** terminal ***/

void Die(const char* s){
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(s); // 根据全局变量errno打印错误信息
    exit(1);
}

void DisableRawMode(){
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1){
        Die("tcsetattr");
    }
}


void EnableRawMode(){
	if(tcgetattr(STDIN_FILENO, &E.orig_termios)) Die("tcgetattr"); //读取当前终端的属性到orig_termios
	atexit(DisableRawMode); //退出program时执行DisableRawMode
	
	struct termios raw = E.orig_termios;

	raw.c_iflag &= ~(ICRNL | IXON); // 关闭Ctrl-M, Ctrl-S, Ctrl-Q
	raw.c_oflag &= ~(OPOST); //  关闭终端的输出处理功能,进制将\n自动转化为\r\n
	raw.c_cflag |= (CS8);
	raw.c_lflag &= ~(BRKINT | ECHO | ICANON | ISIG | IEXTEN); //关闭BRKINT, ECHO，ICANON，ISIG, IEXTEN 标志位，进入Raw Mode
	// 每个标志位是raw.c_lflag二进制中的某个位。因此通过将各个标志进行或运算后取反，再和raw.c_flag并，可以做到只将这几个标志位置0，从而禁用
	// 关闭ECHO ：禁用回显输入字符的功能(不再显示你的输入)
	// 关闭ICANON：禁用规范模式，输入不再按行缓冲，即无需回车键即可立即读取字符
	// 禁用ISIG：关闭Ctrl-C(中断程序)和Ctrl-Z(暂停并放入后台)
	// 禁用IEXTEN：关闭Ctrl-V


	//给read()设置一个timeout
	raw.c_cc[VMIN] = 0;//设置最小读取字符数为0，当到达VTIME后，即使字符数为0也会返回。若输入数据，则会立刻返回
	raw.c_cc[VTIME] = 1;//单位：1/10s。定义read()的最大等待时间


	if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw)) Die("tcsetattr");
}

int EditorReadKey(){
    int nread;
    char c;
    while((nread = read(STDIN_FILENO, &c, 1)) != 1){
        if(nread == -1 && errno != EAGAIN) Die("read");
	}
    if(c == '\x1b'){
        char seq[3];

        if(read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if(read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
        
        if(seq[0] == '['){
            if(seq[1] >= '0' && seq[1] <= '9'){
                if(read(STDIN_FILENO, &seq[2], 1) != 1) 
                    return '\x1b';
                if(seq[2] == '~'){
                    switch(seq[1])
                    {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY; 
                        case '8': return END_KEY;
                    }
                }
            }
            else{
                switch(seq[1]){
                   case 'A': 
                       return ARROW_UP;
                    case 'B':
                        return ARROW_DOWN;
                    case 'C':
                        return ARROW_RIGHT;
                    case 'D':
                        return ARROW_LEFT;
                    case 'H':
                        return HOME_KEY;
                    case 'F':
                        return END_KEY;
                }
            }
        }
        return '\x1b';
    }
    else{
        return c;
    }
}

int GetCursorPosition(int *rows, int *cols){
	char buf[32];
	unsigned int i = 0;

	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1; // 终端查询光标位置的ANSI转义序列。终端在收到这个序列后会返回当前光标的位置
	
	while(i < sizeof(buf) - 1){
	    if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
	    if(buf[i] == 'R') break; // 遇到R表明光标位置读取完毕
	    i++;
	}
	buf[i] = '\0';

	if (buf[0] != '\x1b' || buf[1] != '[') 
        return -1;
	if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) 
        return -1;
    
	return 0;
}

int GetWindowSize(int *rows, int *cols){
	struct winsize ws;
	
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) { // 获取终端窗口大小，并检测列是否为0
	    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) // 光标向右，向下移动999 
	        return -1;
	    return GetCursorPosition(rows, cols);
	}
	else {
	    *cols = ws.ws_col;
	    *rows = ws.ws_row;
	    return 0;
	}
}
/*** syntax highlight ***/

int IsSaparator(int c){ // 判断是否为分隔符，以防止类似int32_t中的32也被识别为数字的情况
    return isspace(c) || c== '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void EditorUpdateSyntax(erow *row){
    row->hl = realloc(row->hl, row->rsize);
    memset(row->hl, HL_NORMAL, row->rsize); // 默认全部设为NORMAL

    if(E.syntax == NULL) return;
    
    char **keywords = E.syntax->keywords;

    char *scs = E.syntax->singleline_comment_start;
    char *mcs = E.syntax->multiline_comment_start;
    char *mce = E.syntax->multiline_comment_end; // 分别是单行注释，多行注释开头，多行注释结尾的缩写
                                                 //
    int scs_len = scs ? strlen(scs) : 0;
    int mcs_len = mcs ? strlen(mcs) : 0;
    int mce_len = mce ? strlen(mce) : 0;

    int prev_sep = 1; // 视一行的最开头为分隔符
    int in_string = 0;
    int in_comment = (row->idx > 0 && E.row[row->idx-1].hl_open_comment); // 前一个有未闭合的多行注释，则此行的in_comment = 1

    int i = 0;
    while(i < row->rsize){
        char c = row->render[i];
        unsigned char prev_hl = (i > 0) ? row->hl[i-1] : HL_NORMAL; // 前一个字符的高亮类型
        
        if(scs_len && !in_string && !in_comment){ // 不在字符串和多行注释内
            if(!strncmp(&row->render[i], scs, scs_len)){ // 判断是否是单行注释
                memset(&row->hl[i],HL_COMMENT, row->rsize - i); // 如果是，把本行的高亮从此处开始全部设置为HL_COMMENT
                break;
            }
        }

        if(mcs_len && mce_len && !in_string){
            if(in_comment){
                row->hl[i] = HL_COMMENT;
                if(!strncmp(&row->render[i], mce, mce_len)){ // 如果遇到了*/, 则高亮*/并跳过，退出多行注释
                    memset(&row->hl[i], HL_MLCOMMENT, mce_len);
                    i += mce_len;
                    in_comment = 0;
                    prev_sep = 1;
                    continue;
                }
                else{
                    i++;
                    continue;
                }
            }
            else if(!strncmp(&row->render[i], mcs, mcs_len)){
                memset(&row->hl[i], HL_COMMENT, mcs_len);
                i += mcs_len;
                in_comment = 1;
                continue;
            }
        }

        if(E.syntax->flags & HL_HIGHLIGHT_STRINGS){
            if(in_string){
                row->hl[i] = HL_STRING;
                if(c == '\\' && i + 1 < row->rsize){ // 转义的处理
                    row->hl[i+1] = HL_STRING;
                    i += 2; // 跳过后一个字符
                    continue;
                }
                if(c == in_string) // 是否是对应的结束引号
                    in_string = 0;
                i++;
                prev_sep = 1;
                continue;
            }
            else{
                if(c == '"' || c == '\''){
                    in_string = c; // 把双/单引号存储进c,好知道用哪个作为结束
                    row->hl[i] = HL_STRING;
                    i++;
                    continue;
                }
            }
        }


        // 条件：
        // 1. 当前字符是数字 且 前一个数字是分隔符 或 前一个字符高亮类型是数字
        // 2. 或 当前字符是小数点 且 再前一个字符的高亮类型是数字
        if(E.syntax->flags & HL_HIGHLIGHT_NUMBERS){ // 判断当前文件类型是否应该高亮数字
            if((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER))
                        || (c == '.' && prev_hl == HL_NUMBER))
            { 
                row->hl[i] = HL_NUMBER;
                i++;
                prev_sep = 0; // 表明在高亮的中途
                continue;
            }
        }

        if(prev_sep){ // 判断关键字的开头是否有分隔符
            int j;
            for(j = 0; keywords[j]; j++){
                int klen = strlen(keywords[j]);
                int kw2 = keywords[j][klen - 1] == '|'; // 用于判断是否属于keywords2的bool
                if(kw2)
                    klen--;

                if(!strncmp(&row->render[i], keywords[j], klen) &&
                        IsSaparator(row->render[i+klen])){ // 判断是关键字且末尾有分隔符
                    memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
                    i += klen;
                    break;
                }
                if(keywords[j] != NULL){
                    prev_sep = 0;
                    continue;
                }
            }
        }

        prev_sep = IsSaparator(c); // 如果没有高亮当前字符，则会执行到while的结尾，此处判断当前字符是否是分隔符
        i++;
    }
    int changed = (row->hl_open_comment != in_comment);
    row->hl_open_comment = in_comment; // 判断当前行是否是未闭合的多行注释(如果in_comment = 1,则是)
    if(changed && row->idx + 1 < E.numrows) //如果改变，则递归更新下一行
        EditorUpdateSyntax(&E.row[row->idx + 1]);
}

int EditorSyntaxToColor(int hl){ //30-黑，31-红，37-白
    switch(hl){
        case HL_COMMENT:
        case HL_MLCOMMENT:
            return 36;
        case HL_KEYWORD1:
            return 33; // yellow
        case HL_KEYWORD2:
            return 32; // green
        case HL_STRING:
            return 35;
        case HL_NUMBER: 
            return 31;
        case HL_MATCH:
            return 34;
        default:
            return 37;
    }
}

void EditorSelectSyntaxHighlight(){
    E.syntax = NULL;
    if(E.filename == NULL) return;

    char *ext = strchr(E.filename, '.'); // 找到最后一次出现的.  ,从而找到扩展名部分

    for(unsigned int j = 0; j < HLDB_ENTRIES; j++){
        struct editorSyntax *s = &HLDB[j];
        unsigned int i = 0;
        while(s->filematch[i]){
            int is_ext = (s->filematch[i][0] == '.');
            // strchr: 返回指向某个字符在字符串中最后一次出现的位置的指针
            // strcmp 在两个字符串相同时返回0
            if((is_ext && ext && !strcmp(ext, s->filematch[i])) || 
                    (!is_ext && strstr(E.filename, s->filematch[i]))){
                E.syntax = s;

                int filerow;
                for(filerow = 0; filerow < E.numrows; filerow++){
                    EditorUpdateSyntax(&E.row[filerow]);
                }
                return;
            }
            i++;
        }
    }
}

/*** row operations ***/

int EditorRowCxToRx(erow *row, int cx){
    int rx = 0;
    int j;
    for(j = 0; j < cx; j++){
        if(row->chars[j] == '\t'){
            rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
        }
        rx++;
    }
    return rx;
}

int EditorRowRxToCx(erow *row, int rx){
    int cur_rx = 0;
    int cx;
    for(cx = 0; cx < row->size; cx++){
        if(row->chars[cx] == '\t')
            cur_rx += (KILO_TAB_STOP - 1) - (cur_rx % KILO_TAB_STOP);
        cur_rx++;

        if(cur_rx > rx) return cx; // 只是为了防止传入的rx超过范围
    }
    return cx;
}

void EditorUpdateRow(erow *row){
    //将tab转化为空格
    int tabs = 0;
    int j;
    for(j = 0; j < row->size; j++){
        if(row->chars[j] == '\t') 
            tabs++;
    }

    free(row->render);
    row->render = malloc(row->size + tabs * (KILO_TAB_STOP - 1) + 1); // 一个tab变KILO_TAB_STOP个空格，因此要加上(KILO_TAB_STOP - 1)*tabs

    int idx = 0;
    for(j = 0; j< row->size;j++){
        if(row->chars[j] == '\t'){
            row->render[idx++] = ' '; //tab会至少加一个空格
            while(idx % KILO_TAB_STOP != 0) // 前进到tab stop(被8整除的col位置)
                row->render[idx++] = ' ';
        }
        else{
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;

    EditorUpdateSyntax(row);
}

void EditorInsertRow(int at, char *s, size_t len) {
    if(at < 0 ||at >E.numrows) return;

    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1)); // 多一行
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

    for(int j = at + 1; j <= E.numrows; j++) // 插入行时，后面的行的idx++
        E.row[j].idx++;

    E.row[at].idx = at;

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    E.row[at].hl = NULL;
    E.row[at].hl_open_comment = 0;
    EditorUpdateRow(&E.row[at]);

    E.numrows++;
    E.dirty++;
}

void EditorFreeRow(erow *row){
    free(row->render);
    free(row->chars);
    free(row->hl);
}

void EditorDelRow(int at){
    if(at < 0 || at >= E.numrows) return;
    EditorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1)); // 把第at + 1 行及之后的都往前挪一行
    for(int j = at; j < E.numrows - 1;j++){
        E.row[j].idx--;
    }
    E.numrows--;
    E.dirty++;
}

void EditorRowInsertChar(erow* row, int at, int c){
    if(at < 0 || at > row->size)
        at = row->size;
    row->chars = realloc(row->chars, row->size + 2); // 原长度为row->size + 1('\0')
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at +1); // 类似memcpy,但更安全. 把从at开始到最后的元素向后移一位，总共row->size + 1 - at个元素被移动
    row->size++; // 更新插入后的行字符数
    row->chars[at] = c; // 插入字符
    EditorUpdateRow(row);
    E.dirty++;
}

void EditorRowDelChar(erow* row, int at){
    if(at < 0 || at >= row->size) return;
    memmove(&row->chars[at], &row->chars[at+1], row->size - at);
    row->size--;
    EditorUpdateRow(row);
    E.dirty++;
}

void EditorRowAppendString(erow *row, char* s, size_t len){
    row->chars = realloc(row->chars, row->size + len + 1); // 扩容
    memcpy(&row->chars[row->size], s, len); // 在原来的row->chars后追加s
    row->size += len;
    row->chars[row->size] = '\0';
    EditorUpdateRow(row);
    E.dirty++;
}

/*** editor operations ***/

void EditorInsertChar(int c){
    if(E.cy == E.numrows){
        EditorInsertRow(E.numrows, "", 0);
    }
    EditorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void EditorInsertNewLine(){
    if(E.cx == 0){// 如果光标在开头，则直接在当前行插入一行空行
        EditorInsertRow(E.cy, "", 0);
    }
    else{//不在开头，则在下一行插入一行，内容为当前光标右侧内容
        erow *row = &E.row[E.cy];
        EditorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';//截断当前行内EditorInsertRow(int at, char *s, size_t len) ss
        EditorUpdateRow(row);
    }
    E.cy++;
    E.cx = 0;
}

void EditorDelChar(){//根据光标位置删除char
    if(E.cy == E.numrows) return; // 在最后一行下方，则无效
    if(E.cx == 0 && E.cy == 0) return; // 最左上角，无效
    
    erow *row = &E.row[E.cy]; // 获取当前行
    if(E.cx > 0){
        EditorRowDelChar(row, E.cx - 1);
        E.cx--;
    }
    else{ // 如果光标在最左侧，则把该行接到上一行末尾
        E.cx = E.row[E.cy - 1].size;
        EditorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
        EditorDelRow(E.cy);
        E.cy--;
    }
}

/*** file i/o ***/

char *EditorRowsToString(int *buflen){
    int totlen = 0;
    int j;
    for(j = 0; j < E.numrows; j++){
        totlen += E.row[j].size + 1; // 获得insert之后的总长度
    }
    *buflen = totlen;

    char *buf = malloc(totlen);
    char *p = buf;
    for(j = 0; j < E.numrows; j++){ // 一行行复制
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }
    return buf;
}

void EditorOpen(char *filename){
    free(E.filename);
    E.filename = strdup(filename); //复制str, 需要自己free

    EditorSelectSyntaxHighlight();

    FILE *fp = fopen(filename, "r");
    if(!fp) Die("fopen");

    char *line = NULL;
    ssize_t linelen;
    size_t linecap = 0; // 指向缓冲区大小的指针，在getline函数中会根据需要被更新

    while((linelen = getline(&line, &linecap, fp)) != -1){
        while(linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
            linelen--;
        EditorInsertRow(E.numrows, line, linelen);
    }
    free(line);
    fclose(fp);
    E.dirty = 0; // 打开时会自动调用EditorInsertRow,使dirty自增,因此在这里重置
}

void EditorSave(){
    if(E.filename == NULL){
        E.filename = EditorPrompt("Save as : %s (Esc to cancel)", NULL); // save as ... 
        if(E.filename == NULL){ // 若还为NULL,说明用户ESC了
            EditorSetStatusMessage("Save aborted");
            return;
        }
        EditorSelectSyntaxHighlight();
    }

    int len;
    char *buf = EditorRowsToString(&len); // buf指向内容为insert之后的总文件内容
    
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644); // O_RDWR:读写模式   O_CREAT:如果文件不存在则创建
                                                       // 0644:文件权限(用户可读写,组和其它只读)
    if(fd != -1){
        if(ftruncate(fd, len) != -1){ // 设置文件长度为len
            if(write(fd, buf, len) == len){
                close(fd);
                //TEST

                //test
                FILE *file = fopen("testSaver.txt", "w");
                fprintf(file, "\n\n file lines :  %d, bytes: %d \n %s \n ", E.numrows, len, buf);
                fclose(file);
                //test end

                //TEST END
                free(buf);
                E.dirty = 0;
                EditorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }
    //test
    FILE *file = fopen("testSaver.txt", "w");
    fprintf(file, "\n\n file lines : , %d \n", E.numrows);
    fclose(file);
    //test end
    free(buf); // 释放buf
    EditorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/*** find ***/

void EditorFindCallback(char *query, int key){
    static int last_match = -1;
    static int direction = 1;

    static int saved_hl_line;
    static char *saved_hl = NULL;
    
    if(saved_hl){ // 搜索结束后恢复
        memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
        free(saved_hl);
        saved_hl = NULL;
    }
    if(key == '\r' || key == '\x1b'){ // reset
        last_match = -1;
        direction = 1;
        return;
    }
    else if(key == ARROW_RIGHT || key == ARROW_DOWN){ // 向后搜索
        direction = 1;
    }
    else if(key == ARROW_LEFT ||key == ARROW_UP){ // 向前搜索
        direction = -1;
    }
    else{
        last_match = -1;
        direction = 1;
    }

    if(last_match == -1) direction = 1;
    int current = last_match; 
    int i;
    for(i = 0; i < E.numrows;i++){
        current += direction; // 如果有last_match,会从下一行开始查找。如果没有,那么last_match = -1, current = 0, 即从头开始寻找
        if(current == -1) // 向前寻找到头了则返回文件尾部
            current = E.numrows - 1;
        else if(current == E.numrows) // 向后寻找到头了则返回文件头部
            current = 0;

        erow *row = &E.row[current];
        char *match = strstr(row->render, query); // 判断query是否是row->render的substr
        if(match){
            last_match = current;
            E.cy = current;
            E.cx = EditorRowRxToCx(row, match - row->render);
            E.rowoff = E.numrows; // 把屏幕拉到最底部，让EditorScroll往上滚，使得匹配的行出现在最顶部
            
            saved_hl_line = current;
            saved_hl = malloc(row->rsize);
            memcpy(saved_hl, row->hl, row->rsize);

            memset(&row->hl[match - row->render], HL_MATCH, strlen(query)); // 从匹配位置开始往后query个长度颜色改为match
            break;
        }
    }
}

void EditorFind(){
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    int saved_coloff = E.coloff;
    int saved_rowoff = E.rowoff;
    char *query = EditorPrompt("Search: %s (Use ESC/Arrows/Enter)", EditorFindCallback);

    if(query)
        free(query);
    else{ // 按ESC时恢复光
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.coloff = saved_coloff;
        E.rowoff = saved_rowoff;
    }
}

/*** append buffer ***/

// 不希望用一堆writes进行输出
// 希望每次都往一个buffer中追加str, 最后只打印一次

typedef struct{
    char *b; //buffer
    int len; //length
} abuf;

#define ABUF_INIT {NULL, 0}    // 表示空的abuf

void abAppend(abuf *ab, const char *s, int len){
    char *new = realloc(ab->b, ab->len + len); //扩容空间/释放并寻找新空间

    if(new == NULL) return;
    memcpy(&new[ab->len], s, len); // 把新内容追加进入
    ab->b = new;
    ab->len += len;
}

void abFree(abuf* ab){
    free(ab->b);
}

/*** input ***/

char *EditorPrompt(char *prompt, void (*callback)(char *, int)){ // prompt 应带有一个%s
    size_t bufsize = 128;
    char *buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';

    while(1){
        EditorSetStatusMessage(prompt, buf);
        EditorRefreshScreen();

        int c = EditorReadKey();
        if(c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE){
            if(buflen != 0) buf[--buflen] = '\0';
        }
        else if(c == '\x1b'){ // 按ESC撤销
            EditorSetStatusMessage("");
            if(callback)
                callback(buf, c);
            free(buf);
            return NULL;
        }
        else if(c == '\r'){
            if(buflen != 0){
                EditorSetStatusMessage("");
                if(callback)
                    callback(buf,c);
                return buf;
            }
        }
        else if(!iscntrl(c) && c < 128){
            if(buflen == bufsize -1){
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }

        if(callback)
            callback(buf, c);
    }
}

void EditorMoveCursor(int key){
    // 光标在最后一行下方时, row 为NULL , switch中无法向右
    // 光标不在最后一行下方时 , 所在行有文本 , 设row为当前所在行 , switch中使向右不超过最后一个字符的后一位
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];  
    switch(key){
        case ARROW_LEFT:
            if(E.cx != 0)
                E.cx--;
            else if(E.cy > 0){ // 在行首按左会移动到上一行的末尾
                --E.cy;
                E.cx = E.row[E.cy].size;
            }
            break;
        case ARROW_RIGHT:
            if(row && E.cx < row->size) //E.cx最大值为row->size, 即最后一个字符的右边一格
                E.cx++;
            else if(row && E.cx == row->size){ // 在行末按右移动到下一行的开头
                E.cy++;
                E.cx = 0;
            }
            break;
        case ARROW_UP:
            if(E.cy != 0)
                E.cy--;
            break;
        case ARROW_DOWN:
            if(E.cy < E.numrows)
                E.cy++;
            break;
    }
    
    // 从长行上下移动到短行时，会将光标置于行末尾
    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0; // 如果是最后一行的下方,长度设为0，否则获取row->size
    if(E.cx > rowlen) // 如果cx大于该行文本长度，移动光标到行末尾
        E.cx = rowlen;
}

void EditorProcessKeypress(){
    static int quit_times = KILO_QUIT_TIMES;

	int c = EditorReadKey();

	switch(c){
        case '\r':
            EditorInsertNewLine();
            break;
        // 退出
        case CTRL_KEY('q'):
            if(E.dirty && quit_times > 0){
                EditorSetStatusMessage("WARNING! File has unsaved changes. "
                        "Press Ctrl-Q %d more times to quit", quit_times);
                quit_times--;
                return;
            }
	        write(STDOUT_FILENO, "\x1b[2J", 4);
	        write(STDOUT_FILENO, "\x1b[H", 3);
	        exit(0);
	        break;
        // 保存
        case CTRL_KEY('s'):
            EditorSave();
            break;
        // 把光标移动到左/右
        case HOME_KEY:
            E.cx = 0;
            break;
        case END_KEY:
            if(E.cy < E.numrows)
                E.cx = E.row[E.cy].size;
            break;
        case CTRL_KEY('f'):
            EditorFind();
            break;
        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if(c == DEL_KEY) EditorMoveCursor(ARROW_RIGHT);
            EditorDelChar();
            break;
        // 把光标移动到首/末
        case PAGE_UP:
        case PAGE_DOWN:
            {   
                if(c == PAGE_UP)
                    E.cy = E.rowoff; // 回到屏顶
                else if(c == PAGE_DOWN){
                    E.cy = E.rowoff + E.screen_rows - 1; // 回到屏底
                    if(E.cy > E.numrows)
                        E.cy = E.numrows;// 如果屏底超过文本行数, 光标回到文本最后一行
                }
                int times = E.screen_rows;
                while(times--){
                    EditorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
                }
                break;
            }
        // 箭头移动光标
        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            EditorMoveCursor(c);
            break;
        case CTRL_KEY('l'):
        case '\x1b':
            break;
        default:
            EditorInsertChar(c);
            break;
	}
    quit_times = KILO_QUIT_TIMES; // 按Ctrl-Q以外的键就会恢复为KILO_QUIT_TIMES
}

/*** output ***/

void EditorScroll(){
    E.rx = 0;
    if(E.cy < E.numrows){
        E.rx = EditorRowCxToRx(&E.row[E.cy], E.cx);
    }
    if(E.cy < E.rowoff){// top
        E.rowoff = E.cy;
    }
    if(E.cy >= E.rowoff + E.screen_rows){// bottom
        E.rowoff = E.cy - E.screen_rows + 1;
    }
    if(E.rx < E.coloff){
        E.coloff = E.rx;
    }
    if(E.rx > E.coloff + E.screen_cols){
        E.coloff = E.rx - E.screen_cols + 1;
    }
}

void EditorDrawStatusBar(abuf *ab){
    abAppend(ab, "\x1b[7m", 4); //反转颜色. \x1b[ + [0: clear attr, 1:bold, 4:underscore, 5:blink, 7:inverted color] + m
    char status[80], rstatus[80];
    // len: 写入长度, status: 目标缓冲区, %.20s : 最多输出20个字符串
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", 
            E.filename ? E.filename : "[No Name]",
            E.numrows,
            E.dirty ? "(modified)" : ""); // file信息
    int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d",
            (E.syntax ? E.syntax->filetype : "no ft"), E.cy + 1, E.numrows); // 当前file type 和 行号信息
    if(len > E.screen_cols)
        len = E.screen_cols;
    abAppend(ab, status, len);

    while(len < E.screen_cols){
        if(E.screen_cols - len == rlen){
            abAppend(ab, rstatus, rlen);
            break;
        }
        else{
            abAppend(ab, " ", 1);
            len++;
        }
    }
    abAppend(ab, "\x1b[m", 3); // 恢复颜色
    abAppend(ab, "\r\n", 2);
}

void EditorDrawMessageBar(abuf *ab){
    abAppend(ab, "\x1b[K", 3); // 清空msg
    int msglen = strlen(E.statusmsg);
    if(msglen > E.screen_cols) 
        msglen = E.screen_cols;
    if(msglen && time(NULL) - E.statusmsg_time < 5) // 只显示5s内的msg
        abAppend(ab, E.statusmsg, msglen);
}

void EditorDrawRows(abuf *ab){
	int y;
	for (y = 0; y < E.screen_rows; y++){
        int filerow = y + E.rowoff;
        if(filerow >= E.numrows){
            if(E.numrows == 0 && y == E.screen_rows / 3){ // 未打开文件时才显示欢迎
	            char welcome[80];
	            int welcomelen = snprintf(welcome, sizeof(welcome),
	                    "Kilo editor -- version %s", KILO_VERSION);
	            if(welcomelen > E.screen_cols) 
	                welcomelen = E.screen_cols;
	            //居中显示
	            int padding = (E.screen_cols - welcomelen) / 2;
	            if(padding){
	                abAppend(ab, "~", 1);
	                padding--;
	            }
	            while(padding--) abAppend(ab, " ", 1);
	            abAppend(ab, welcome, welcomelen);
      	  	}
       	    else{
                abAppend(ab, "~", 1);
            }
        }
        else{
            int len = E.row[filerow].rsize - E.coloff;
            len = len < 0 ? 0 : len;
            if (len > E.screen_cols) 
                len = E.screen_cols;
            char *c = &E.row[filerow].render[E.coloff];
            unsigned char *hl = &E.row[filerow].hl[E.coloff];
            int current_color = -1;
            int j;
            for(j = 0; j < len; j++){
                if(iscntrl(c[j])){ // 检查是否是控制字符
                    char sym = (c[j] <= 26) ? '@' + c[j] : '?'; // @后面就是A，通过'@' + c[j] 把控制字符转化成字母打印，如果不在字母范围内，就用?代替
                    abAppend(ab, "\x1b[7m", 4); // 反色
                    abAppend(ab, &sym, 1);
                    abAppend(ab, "\x1b[m", 3); // 恢复

                    if (current_color != -1) {
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", current_color);
                        abAppend(ab, buf, clen);
                    }
                }
                else if(hl[j] == HL_NORMAL){ // 单独处理Normal情况
                    if(current_color != -1){
                        current_color = -1;
                        abAppend(ab, "\x1b[39m", 5);
                    }
                    abAppend(ab, &c[j], 1);
                }
                else{
                    int color = EditorSyntaxToColor(hl[j]); // hl[j]颜色对应的ANSI控制字段中的数字部分
                    if(color != current_color){
                        current_color = color;
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color); //利用格式化字符串把数字放进去 
                        abAppend(ab, buf, clen); // 在append实际字符前先append颜色控制的ANSI序列
                    }
                    abAppend(ab, &c[j], 1);
                }
            }
      	    abAppend(ab, "\x1b[39m", 5);	
        }
        abAppend(ab, "\x1b[K", 3); // 清屏时每次删除一行而不是直接清屏
        abAppend(ab, "\r\n", 2);
    }
}

void EditorRefreshScreen() {
    EditorScroll();

    abuf ab = ABUF_INIT;
    
    abAppend(&ab, "\x1b[?25l", 6); // Draw之前隐藏光标
	//abAppend(&ab, "\x1b[2J", 4); // 利用转义字符清屏   4代表4bytes
	abAppend(&ab, "\x1b[H", 3);  // 利用转义字符移动光标到左上角
	
	EditorDrawRows(&ab);
    EditorDrawStatusBar(&ab);
    EditorDrawMessageBar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);  //移动光标
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6); // 重新显示光标
    
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void EditorSetStatusMessage(const char *fmt, ...){ // 可变参数列表
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/*** init ***/

void InitEditor(){
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;
    E.syntax = NULL;

	if(GetWindowSize(&E.screen_rows, &E.screen_cols) == -1) Die("GetWindowSize");
    E.screen_rows -= 2; //空出2行用于显示状态
}


int main(int argc, char* argv[]) // 命令传参相关
{
	EnableRawMode();

	InitEditor();

    if(argc >= 2){
        EditorOpen(argv[1]);
    }

    EditorSetStatusMessage("HELP:= Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");
	while(1){
		EditorRefreshScreen();
		EditorProcessKeypress();
	}

	return 0;
}
