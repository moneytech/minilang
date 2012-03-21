#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>


enum {
	LEX_EOF = EOF,
	LEX_IF = 0,
	LEX_ELSE,
	LEX_ELIF,
	LEX_END,
	LEX_WHILE,
	LEX_BREAK,
	LEX_CONTINUE,
	LEX_RETURN,
	LEX_KEYWORD_COUNT,
	LEX_CHAR,
	LEX_STRING,
	LEX_NUMBER,
	LEX_IDENT,
	LEX_SIZE
};

const char* keywords[] = { "if", "else", "elif", "end", "while", "break",
	"continue", "return", NULL, NULL, NULL, "number", "identifier" };
const char* call_regs[] = { "rdi", "rsi", "rdx", "rcx", "r8", "r9" };


//	scanner
int			character;
int			lexeme;
char		token[1024];
long long	number;
int			neg_number;
int			line_number;
int			cursor_pos;

FILE*		src_file;
FILE*		dst_file;


void error(char* msg, ...) {
	fprintf(stderr, "%d:%d: error: ", line_number, cursor_pos);
	va_list args;
	va_start(args, msg);
	vfprintf(stderr, msg, args);
	va_end(args);
	fprintf(stderr, "\n");
	exit(1);
}


void output(char* msg, ...) {
	va_list args;
	va_start(args, msg);
	vfprintf(dst_file, msg, args);
	va_end(args);
}


int read_char() {
	int c = character;
	character = fgetc(src_file);
	cursor_pos++;
	if(character == '\n') {
		line_number++;
		cursor_pos = 0;
	}
	return c;
}


int scan() {

	while(isspace(character)) read_char();

	// ignore comment
	if(character == '/') {
		read_char();
		if(character != '/') return '/';
		while(read_char() != '\n') {}
		while(isspace(character)) read_char();
	}

	// one character token
	if(strchr("-+*%&|^~!=<>;:(),", character)) {
		int c = character;
		read_char();
		neg_number = (c == '-' && isdigit(character));
		return c;
	}

	// TODO: char

	// string
	if(character == '"') {
		int i = 0;
		do {
			if(character == '\\') token[i++] = read_char();
			token[i++] = read_char();
			if(i > 1020) error("string too long");
		} while(character != '"');
		token[i++] = read_char();
		token[i] = '\0';
		return LEX_STRING;
	}

	// number
	if(isdigit(character)) {
		int i = 0;
		do {
			token[i++] = read_char();
			if(i > 20) error("number too long");
		} while(isdigit(character));
		token[i] = '\0';
		number = atoll(token);
		return LEX_NUMBER;
	}

	if(isalpha(character) || character == '_') {
		int i = 0;
		do {
			token[i++] = read_char();
			if(i > 62) error("identifier too long");
		} while(isalnum(character) || character == '_');
		token[i] = '\0';

		// check for keywords
		for(i = 0; i < LEX_KEYWORD_COUNT; i++) {
			if(strcmp(token, keywords[i]) == 0) return i;
		}
		return LEX_IDENT;
	}

	if(character != EOF) error("unknown character");
	return LEX_EOF;
}


void read_lexeme() {
	lexeme = scan();
}


void init_scanner() {
	line_number = 1;
	read_char();
	read_lexeme();
}


void expect(int l) {
	if(lexeme != l) {
		if(l < LEX_SIZE) error("%s expected", keywords[l]);
		else error("%c expected", l);
	}
	read_lexeme();
}


// symbol table
typedef struct {
	char	name[64];
	int		offset;
} Variable;

Variable	locals[1024];
int			local_count;

int			frame;
int			param_count;

int			label;

const char*	regs[] = { "r8", "r9", "r11", "rax" };
enum {
			cache_size = sizeof(regs) / sizeof(char*)
};
int			cache[cache_size];
int			stack_size;


void init_cache() {
	for(int i = 0; i < cache_size; i++) cache[i] = i;
	stack_size = 0;
}


Variable* lookup_variable(char* name, Variable* table, int size) {
	for(int i = 0; i < size; i++) {
		if(strcmp(name, table[i].name) == 0) return &table[i];
	}
	return NULL;
}


Variable* lookup_local(char* name) {
	return lookup_variable(name, locals, local_count);
}


void add_variable(char* name, int offset, Variable* table, int size) {
	for(int i = 0; i < 1024; i++) {
		if(i == size) {
			strcpy(table[i].name, name);
			table[i].offset = offset;
			return;
		}
		if(strcmp(name, table[i].name) == 0) error("multiple declarations");
	}
	error("too many variables");
}


void add_local(char* name, int offset) {
	add_variable(name, offset, locals, local_count);
	local_count++;
}


void param_decl() {
	expect(LEX_IDENT);
	frame += 8;
	add_local(token, frame);
}


void push() {
	int i = cache_size - 1;
	int tmp = cache[i];
	if(stack_size >= cache_size) output("\tpush %s\n", regs[tmp]);
	while(i > 0) {
		cache[i] = cache[i - 1];
		i--;
	}
	cache[0] = tmp;
	stack_size++;
}


void pop() {
	stack_size--;
	if(stack_size == 0) init_cache();
	else {
		int i = 0;
		int tmp = cache[0];
		while(i < cache_size - 1) {
			cache[i] = cache[i + 1];
			i++;
		}
		cache[i] = tmp;
		if(stack_size >= cache_size) output("\tpop %s\n", regs[i]);
	}
}


int is_expr_beginning() {
	static const int lexemes[] = { '-', '!', '(',
		LEX_NUMBER, LEX_STRING, LEX_IDENT };
	for(int i = 0; i < sizeof(lexemes) / sizeof(int); i++)
		if(lexeme == lexemes[i]) return 1;
	return 0;
}


int is_stmt_beginning() {
	return is_expr_beginning() ||
		lexeme == LEX_IF ||
		lexeme == LEX_WHILE ||
		lexeme == LEX_BREAK ||
		lexeme == LEX_CONTINUE ||
		lexeme == LEX_RETURN;
}


void expression() {

	// monadic operands
	if(lexeme == '!') {
		read_lexeme();
		expression();
		output("\ttest %s, %s\n", regs[cache[0]], regs[cache[0]]);
		output("\tsetz cl\n");
		output("\tmovzx %s, cl\n", regs[cache[0]]);
		return;
	}
	if(lexeme == '-') {
		if(!neg_number) {
			read_lexeme();
			expression();
			output("\tneg %s\n", regs[cache[0]]);
			return;
		}
		read_lexeme();
		push();
		output("\tmov %s, %ld\n", regs[cache[0]], -number);
		expect(LEX_NUMBER);
	}
	else if(lexeme == LEX_NUMBER) {
		push();
		output("\tmov %s, %ld\n", regs[cache[0]], number);
		read_lexeme();
	}
	else if(lexeme == '(') {
		read_lexeme();
		expression();
		expect(')');
	}
	else if(lexeme == LEX_IDENT) {
		Variable* v = lookup_local(token);

		read_lexeme();
		if(lexeme == '(') {	// function call
			char name[64];
			strcpy(name, token);



			// TODO: save and restore cache
/*			// push currently used registers
			int used_regs = (stack_size > cache_size) ? cache_size : stack_size;
			for(int i = used_regs - 1; i >= 0; i--)
				output("\tpush %s\n", regs[cache[i]]);
*/
//			for(int i = 0; i < cache_size; i++) push();


			int old_size = stack_size;
			stack_size = 0;

			// expr list
			int args = 0;
			read_lexeme();
			if(is_expr_beginning()) {
				args++;
				expression();
				output("\tpush %s\n", regs[cache[0]]);
				pop();
				while(lexeme == ',') {
					read_lexeme();
					args++;
					if(args > 6) error("too many arguments");
					expression();
					output("\tpush %s\n", regs[cache[0]]);
					pop();
				}
			}
			expect(')');


			// just checking...
			assert(stack_size == 0);


			// set up registers
			for(int i = args - 1; i >= 0; i--)
				output("\tpop %s\n", call_regs[i]);
			output("\txor rax, rax\n");

			// call
			output("\tcall %s\n", name);

			// return value in rax
			init_cache();
			push();
			stack_size = old_size + 1;

		}
		else if(lexeme == '[') {
			error("not implementet yet");
		}
		else if(lexeme == ':') {	// assignment
			if(!v) error("variable not found");
			read_lexeme();
			expression();
			output("\tmov QWORD PTR [rbp - %d], %s\n", v->offset, regs[cache[0]]);
		}
		else {
			if(!v) error("variable not found");
			push();
			output("\tmov %s, QWORD PTR [rbp - %d]\n", regs[cache[0]], v->offset);
		}
	}
	else if(lexeme == LEX_STRING) {
		push();
		output("\t.section .rodata\n");
		output("LC%d:\n", label);
		output("\t.string %s\n", token);
		output("\t.text\n");
		output("\tmov %s, OFFSET LC%d\n", regs[cache[0]], label);
		label++;
		read_lexeme();
	}
	else error("bad expression");



	// dyadic operands
	if(lexeme == '+') {
		read_lexeme();
		expression();
		output("\tadd %s, %s\n", regs[cache[1]], regs[cache[0]]);
		pop();
	}
	else if(lexeme == '-') {
		read_lexeme();
		expression();
		output("\tsub %s, %s\n", regs[cache[1]], regs[cache[0]]);
		pop();
	}


}


void statement_list();


void statement() {
	if(lexeme == LEX_IF) {


	}
	else if(lexeme == LEX_WHILE) {


	}
	else if(lexeme == LEX_BREAK) {


	}
	else if(lexeme == LEX_CONTINUE) {


	}
	else if(lexeme == LEX_RETURN) {
		read_lexeme();
		if(is_expr_beginning()) {
			expression();
			if(strcmp(regs[cache[0]], "rax") != 0)
				output("\tmov rax, %s\n", regs[cache[0]]);
		}
		output("\tleave\n");
		output("\tret\n");
	}
	else if(is_expr_beginning()) {
		expression();
		pop();
	}
}


void statement_list() {
	while(is_stmt_beginning())
		statement();
}


void minilang() {
	output("\t.intel_syntax noprefix\n");
	output("\t.text\n");

	while(lexeme != LEX_EOF) {
		expect(LEX_IDENT);


		// only functions in global scope for now
		output("\t.globl %s\n", token);
		output("%s:\n", token);
		output("\tpush rbp\n");
		output("\tmov rbp, rsp\n");

		frame = 0;
		local_count = 0;

		int params = 0;
		expect('(');
		if(lexeme == LEX_IDENT) {
			params++;
			param_decl();
			while(lexeme == ',') {
				read_lexeme();
				params++;
				if(params > 6) error("too many arguments");
				param_decl();
			}
		}
		expect(')');

		if(frame > 0) output("\tsub rsp, %d\n", frame);
		for(int i = 0; i < params; i++) {
			output("\tmov QWORD PTR [rbp - %d], %s\n", i * 8 + 8, call_regs[i]);
		}

		init_cache();
		statement_list();
		expect(LEX_END);

		output("\tleave\n");
		output("\tret\n");
	}
}


void cleanup() {
	if(src_file) fclose(src_file);
	if(dst_file && dst_file != stdin) fclose(dst_file);
}


int main(int argc, char** argv) {

	if(argc < 2 || argc > 3) {
		printf("usuage: %s <source> [output]\n", argv[0]);
		exit(0);
	}

	src_file = fopen(argv[1], "r");
	if(!src_file) error("opening source file failed");

	if(argc == 3) {
		dst_file = fopen(argv[2], "w");
		if(!src_file) error("opening output file failed");
	}
	else dst_file = stdout;

	atexit(cleanup);


	init_scanner();

	label = 0;

	minilang();


	return 0;
}

