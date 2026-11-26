// 测试工具：使用正式 Mooncake C 头文件和动态链接，仅写入指定测试命名空间。
#include <store_c.h>
#include <openssl/sha.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <cstring>
#include <cstdint>
#include <stdexcept>
#include <limits>
#include <algorithm>
#include <cstdlib>
// Register one reusable slot per layer; put_from avoids the SDK's fixed staging pool.
class RegisteredBuffer {
    mooncake_store_t store_;
    void *data_ = nullptr;
public:
    RegisteredBuffer(mooncake_store_t store, size_t size) : store_(store) {
        const auto limit = size_t(std::numeric_limits<ptrdiff_t>::max());
        if (size > limit - 4095) throw std::runtime_error("value size overflow");
        const size_t aligned = (size + 4095) / 4096 * 4096;
        if (posix_memalign(&data_, 4096, aligned) != 0) throw std::bad_alloc();
        if (mooncake_store_register_buffer(store_, data_, aligned) != 0) {
            free(data_); data_ = nullptr;
            throw std::runtime_error("register buffer failed");
        }
    }
    RegisteredBuffer(const RegisteredBuffer &) = delete;
    RegisteredBuffer &operator=(const RegisteredBuffer &) = delete;
    ~RegisteredBuffer() { mooncake_store_unregister_buffer(store_, data_); free(data_); }
    char *data() { return static_cast<char *>(data_); }
};
static void le64(char *p, uint64_t n) { for (int i=0;i<8;i++) p[i]=(n>>(8*i))&255; }
int main(int argc, char **argv) {
    if(argc!=7) { std::cerr<<"usage: pool-seed master metadata local-ip namespace tenant layer-list\n"; return 2; }
    auto store=mooncake_store_create();
    if(!store) return 1;
    int result=0;
    try {
        if(mooncake_store_setup(store,argv[3],argv[2],0,16*1024*1024,"tcp","",argv[1])!=0)
            throw std::runtime_error("Mooncake setup failed");
        std::ifstream list(argv[6]); if(!list) throw std::runtime_error("cannot open layer list");
        std::string line; uint64_t blocks=0,bytes=0;
        while(std::getline(list,line)) {
            std::istringstream row(line); std::string digest,path; size_t bs;
            if(!(row>>digest>>bs>>path) || bs<4096 || bs > (uint64_t(INT64_MAX) - 4096) / 4096 * 4096 || bs > uint64_t(std::numeric_limits<ptrdiff_t>::max()) - 4096 || bs%4096)
                throw std::runtime_error("invalid layer list");
            std::ifstream input(path,std::ios::binary); if(!input) throw std::runtime_error("cannot open layer");
            input.seekg(0, std::ios::end);
            auto file_size = input.tellg();
            if (file_size <= 0) throw std::runtime_error("empty or unreadable layer");
            input.seekg(0);
            const size_t capacity = std::min<uint64_t>(bs, static_cast<uint64_t>(file_size));
            RegisteredBuffer value(store, capacity + 64); uint64_t offset=0;
            while(input.read(value.data()+64,capacity) || input.gcount()) {
                size_t n=input.gcount();
                std::string key=std::string(argv[4])+"/v1/t/"+argv[5]+"/"+digest+"/"+std::to_string(bs)+"/"+std::to_string(offset/bs);
                memset(value.data(),0,64); memcpy(value.data(),"OBDPOOL1",8);
                le64(value.data()+8,n); le64(value.data()+16,offset);
                SHA256_CTX ctx; SHA256_Init(&ctx); SHA256_Update(&ctx,key.data(),key.size());
                SHA256_Update(&ctx,value.data(),24); SHA256_Update(&ctx,value.data()+64,n);
                SHA256_Final(reinterpret_cast<unsigned char*>(value.data()+24),&ctx);
                int rc=mooncake_store_put_from(store,key.c_str(),value.data(),64+n,nullptr);
                if(rc!=0 && rc!=-705) throw std::runtime_error("put failed: "+key+", rc="+std::to_string(rc));
                offset+=n;bytes+=n;blocks++;
            }
            if(!input.eof()) throw std::runtime_error("layer read failed");
            std::cout<<"已写入 "<<digest<<"，"<<offset<<" bytes\n"<<std::flush;
        }
        std::cout<<"缓存填充完成："<<blocks<<" blocks，"<<bytes<<" bytes\n";
    } catch(const std::exception &e) { std::cerr<<e.what()<<"\n"; result=1; }
    mooncake_store_destroy(store); return result;
}
