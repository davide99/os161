#include <unistd.h>

int main(void) {
	char str[] = "Scrivi qualcosa:\n";
	write(STDOUT_FILENO, str, sizeof str);
	
	char buf[50];
	read(STDIN_FILENO, &buf, 50);
	write(STDOUT_FILENO, &buf, 50);
	
	return 0;
}
