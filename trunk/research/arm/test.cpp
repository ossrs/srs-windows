/*
# see: https://ossrs.net/lts/zh-cn/docs/v4/doc/arm
 arm-linux-gnueabi-g++ -o test test.cpp -static
 arm-linux-gnueabi-strip test
*/
#include <stdio.h>

int main(int argc, char** argv) {
	printf("hello, arm!\n");
	return 0;
}
