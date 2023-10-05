# BoBpiler-forkserver

gcc/clang BoBpiler Forkserver

### Quick Start
```bash
./clang-18 bob.c                
fork client hello
fork server hello # stdin
done
10 # stdin
time_out_set 10
./uuid0/hello0.c # stdin
{    "binary_base": "./uuid0/clang_",    "result": {        "O0": "0",        "O1": "0",        "O2": "0",        "O3": "0",    }}
./_uuid2/driver.c|./_uuid2/func.c # stdin
{    "binary_base": "./_uuid2/clang_",    "result": {        "O0": "0",        "O1": "0",        "O2": "0",        "O3": "0",    }}
exit # stdin
Compiler Exit!
```

## Protocol
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
