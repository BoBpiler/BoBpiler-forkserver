import subprocess
import json

compile_time_out = "10\n"

def sync_write_data(compiler, data:str) :
    compiler.stdin.write(data)
    compiler.stdin.flush()

# C 프로그램 실행
gcc_compiler = subprocess.Popen(["/home/dongFiles/compiler_trunk/llvm-project/build/bin/clang-18", "bob.c"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

def fork_handshake(compiler) -> bool:
    ret = compiler.stdout.readline()
    if ret not in "fork client hello\n" : 
        print("fork client hello failed")
        return False
    sync_write_data(gcc_compiler,"fork server hello\n")
    ret = compiler.stdout.readline()
    if ret not in "done\n" :
        print("fork server hello failed\n")
        return False
    # set timeout
    sync_write_data(gcc_compiler, compile_time_out)
    ret = compiler.stdout.readline()
    if "time_out_set" not in ret:
        print("failed to set time out\n")
        return False 
    print(ret)
    return True

fork_handshake(gcc_compiler)

source_codes = ["./uuid0/hello0.c\n", "./uuid2/driver.c|./uuid2/func.c\n","./uuid1/hello1.c\n"]

# input Source Code File Name
for src in source_codes:
    sync_write_data(gcc_compiler, src)
    line = gcc_compiler.stdout.readline()
    # remove ,
    new_line = line[:-8] + line[-5:]
    data_dict = json.loads(new_line)
    print(data_dict)


exit_cmd = f"exit\n"
gcc_compiler.stdin.write(exit_cmd)
gcc_compiler.stdin.flush()
line = gcc_compiler.stdout.readline()
print(line)

gcc_compiler.terminate()

