# crt-search

Required Dependency: 

postgresql-libs


How to build?

```bash
gcc -O3 -march=native -mtune=native -flto -pipe \
    -fomit-frame-pointer -falign-functions=32 -fno-plt -ffast-math \
    -static-libgcc -static-libstdc++ \
    -Wl,-O1 -Wl,--as-needed -Wl,--strip-all \
    -o crt-search crt-search.c -lpq
```
