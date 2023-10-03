import subprocess
import threading
import json
import queue
import concurrent.futures


run_queue = queue.Queue()
compile_time_out = "5\n"
is_running_system = True
binary_timeout = 10
running_queue_time_out = 60

def parse_response(line:str) -> (str, json):
    if line in "compile exit!\n" :
        return "exit", None
    parts = line.split('|')
    if len(parts) == 3:
        file_name = parts[1]
    else:
        file_name = parts[0]

    start_index = line.find("{")
    end_index = line.rfind("}") + 1
    json_str = line[start_index:end_index]

    # 마지막 콤마 제거
    if json_str[-2] == ',':
        json_str = json_str[:-2] + json_str[-1]

    # JSON 파싱
    return file_name, json.loads(json_str)


def reader_thread(pipe):
    while True:
        line = pipe.readline()
        if line:
            file_name, json_data = parse_response(line)
            if file_name in "exit":
                break
            run_queue.put((file_name, json_data))

def async_write_data(process:subprocess.Popen, data:str) :
    process.stdin.write(data)
    process.stdin.flush()

# Compiler 실행
def run_compiler(compiler:str) -> subprocess.Popen:
    return subprocess.Popen([compiler, "bob.c"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

def fork_handshake(process:subprocess.Popen) -> bool:
    ret = process.stdout.readline()
    if ret not in "fork client hello\n" : 
        print("fork client hello failed")
        return False
    async_write_data(process, "fork server hello\n")
    ret = process.stdout.readline()
    if ret not in "done\n" :
        print("fork server hello failed\n")
        return False
    # set timeout
    async_write_data(process, compile_time_out)
    ret = process.stdout.readline()
    if "time_out_set" not in ret:
        print("failed to set time out\n")
        return False
    print("success set time out!")
    return True


# Exit Compiler
def exit_compiler(process, read_thread):
    exit_cmd = f"exit\n"
    process.stdin.write(exit_cmd)
    process.stdin.flush()

    read_thread.join()
    process.terminate()

# Create Compiler pipe read Thread
def create_pipe_read_thread(process) -> threading.Thread:
    pipe_read_thread = threading.Thread(target=reader_thread, args=(process.stdout,))
    pipe_read_thread.start()
    return pipe_read_thread

def make_return_value(r, opt_level, json_data, compiler):
    ret = {}
    if isinstance(r, subprocess.CompletedProcess):  # r이 CompletedProcess 타입인 경우를 확인
        ret['ret_code'] = str(r.returncode)
        ret['stdout'] = str(r.stdout)
        ret['stderr'] = str(r.stderr)
    elif type(r) is str:
        ret['ret_code'] = json_data[opt_level]
        ret['stdout'] = r
        ret['stderr'] = ""
    else : # Exception인 경우
        ret['ret_code'] = r
        ret['stdout'] = ""
        ret['stderr'] = ""
    return ret

def parse_compiler_name(file_name)->str :
    # "./uuid0/gcc_"
    # "./uuid0/clang_"
    return file_name.split('/')[-1].split('_')[0]

def process_data(opt_level, json_data, file_name):
    compiler_name = parse_compiler_name(file_name)
    if json_data[opt_level] != '0':
        # 여기서 비정상 컴파일인 경우 오류 처리
        return make_return_value("abnormal compile", opt_level, json_data, compiler_name)
    file_path = file_name + str(opt_level)
    try:
        r = subprocess.run(*[file_path], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, timeout=binary_timeout)
    except (subprocess.TimeoutExpired, UnicodeDecodeError, subprocess.SubprocessError, Exception) as e:
        r = e
    return make_return_value(r, opt_level, json_data, compiler_name)

def running_system():
    while is_running_system:
        try:
            file_name, json_data = run_queue.get(timeout=running_queue_time_out)
        except queue.Empty:
            print("running_get timeout")
            continue
        #print(file_name)
        #print(json_data)

        with concurrent.futures.ThreadPoolExecutor() as executor:
            results = list(executor.map(lambda opt_level: process_data(opt_level, json_data, file_name), json_data))
        
        for ret in results:
            print("retcode:", ret['ret_code'])
            print("stdout:", ret['stdout'])
            print("stderr:", ret['stderr'])
        
def create_running_thread() -> threading.Thread :
    running_thread = threading.Thread(target=running_system)
    running_thread.start()
    return running_thread

def exit_running_thread(run_thread:threading.Thread) :
    global is_running_system
    is_running_system = False
    run_thread.join()


gcc_compiler = run_compiler('./gcc-trunk')
#clang_compiler = run_compiler('/home/dongFiles/compiler_trunk/llvm-project/build/bin/clang-18')

def compile_system():
    # start running system

    # fork handshake
    if fork_handshake(gcc_compiler) == False :
        print("gcc fork handshake failed")
        exit(1)
    else :
        print("gcc fork handshake done!")

    ## fork handshake
    #if fork_handshake(clang_compiler) == False :
    #    print("clang fork handshake failed")
    #    exit(1)
    #else :
    #    print("clang fork handshake done!")

    pipe_read_thread_gcc = create_pipe_read_thread(gcc_compiler)
    running_thread = create_running_thread()
    #pipe_read_thread_clang = create_pipe_read_thread(clang_compiler)
    
    # Try to Compile !
    #for i in range(3):
    source_code = f"./_uuid{2}/driver.c|./_uuid{2}/func.c\n"  # Add a newline to indicate the end of the line
    async_write_data(gcc_compiler, source_code)
        #async_write_data(clang_compiler, source_code)

    
    # exit compiler
    exit_compiler(gcc_compiler, read_thread=pipe_read_thread_gcc)
    #exit_compiler(clang_compiler, read_thread=pipe_read_thread_clang)

    exit_running_thread(running_thread)


compile_system()