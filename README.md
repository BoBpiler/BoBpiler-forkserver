# BoBpiler-forkserver

gcc/clang BoBpiler Forkserver

### Fork handshake
```bash
1. Forkserver -> fuzzer.py
"fork client hello\n"
2. Fuzzer.py -> forkserver
"fork server hello\n"
3. Forkserver -> fuzzer.py
"done\n"
4. Fuzzer.py.
"50\n" # set timeout sec
5. Forkserver -> fuzzer.py
"time_out_set 50\n" # echo timeout sec
```

### How to Input Source Code
```bash
# if csmith
"./output/csmith/uuid0/uuid9.c\n"
# if yarpgen
"./output/yarpgen/uuid0/driver.c|./output/yarpgen/uuid0/func.c\n"
```


### Forkserver Result
```JSON
{
  "binary_base" : "./output/csmith/uuid0/clang_",
  "result" : {
                "O0" : "0",
                "O1" : "0",
                "O2" : "0",
                "O3" : "14",
              }
}
```


### Forkserver Internal Error
```JSON
{
	"exit_code" : "0",
	"error_message" : "string"
}
```
