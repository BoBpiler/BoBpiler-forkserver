import subprocess
import threading
import json

def reader_thread(pipe):
    while True:
        line = pipe.readline()
        if line:
            #print("read", line)
            if line in "compile exit!\n" :
                print(line)
                break
            file_name = line.split('|')[0]
            start_index = line.find("{")
            end_index = line.rfind("}") + 1
            json_str = line[start_index:end_index]
            
            # 마지막 콤마 제거
            if json_str[-2] == ',':
                json_str = json_str[:-2] + json_str[-1]
            
            # JSON 파싱
            data = json.loads(json_str)
            print(data)
            print(file_name)
            for d in data:
                file_path = file_name + str(d)
                r = subprocess.run(*[file_path], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
                print(r.stdout)
        else:
            break


# C 프로그램 실행
process = subprocess.Popen(["./gcc-trunk", "bob.c", "-o", "out", "-O0"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

def fork_handshake():
    ret = process.stdout.readline()
    if ret not in "fork client hello\n" : 
        print("fork client hello failed")
        exit(1)
    process.stdin.write("fork server hello\n")
    process.stdin.flush()
    ret = process.stdout.readline()
    if ret not in "done\n" :
        print("fork server hello failed\n")
        exit(1)
    print("fork handshake done!")

fork_handshake()


stdout_thread = threading.Thread(target=reader_thread, args=(process.stdout,))

stdout_thread.start()


# input Source Code File Name
for i in range(3):
    source_code = f"./uuid{i}/hello{i}.c\n"  # Add a newline to indicate the end of the line
    process.stdin.write(source_code)
    process.stdin.flush() # Ensure that the data is actually sent to the C program


exit_cmd = f"exit\n"
process.stdin.write(exit_cmd)
process.stdin.flush()

stdout_thread.join()
process.terminate()

