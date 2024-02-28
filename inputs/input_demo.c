int foo(int a, int b) {
    int k = 10;
    int i;

    for(i = 0; i < 3; i++) {
        a += b;
    }

    return (a+b)*k;
}


int main() {
    int a = 1;
    int b = 2;
    int c, d;

    c = foo(a, b);

    return 0;
}