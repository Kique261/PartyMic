// Disk/queue checks only. No microphones, render endpoints or target processes.
#include "windows/diagnostics.h"
#include <windows.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>
#include <stdexcept>
using namespace partymic;
void require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
int main() {
    const auto root=std::filesystem::temp_directory_path()/("PartyMic-log-test-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    try {
        std::filesystem::create_directories(root);
        diagnostics::start("diagnostics-test",root/"logs");
        std::vector<std::thread> producers;
        const auto begin=GetTickCount64();
        for(int i=0;i<4;++i) producers.emplace_back([]{
            for(int n=0;n<3000;++n) diagnostics::write("concurrent.event",std::string(900,'x'));
        });
        for(auto& thread:producers)thread.join();
        require(GetTickCount64()-begin<10000,"Logging producers took too long");
        require(diagnostics::flush(5000),"Cannot flush accepted concurrent records");
        // Force rollover without depending on how many burst records were dropped.
        for(int i=0;i<6500;++i){
            diagnostics::write("rotate.event",std::string(1800,'y'));
            if(i%32==0)require(diagnostics::flush(3000),"Cannot flush rollover batch");
        }
        for(int i=0;i<16;++i){diagnostics::write("test.final","line1\nline2\rfinish");Sleep(1);}
        require(diagnostics::flush(3000),"Cannot flush final marker");diagnostics::stop();
        size_t files=0;bool marker=false;
        for(const auto& entry:std::filesystem::directory_iterator(root/"logs")){
            ++files;require(entry.file_size()<=2*1024*1024+2500,"Rotated file exceeds bound");
            std::ifstream file(entry.path());std::string line;
            while(std::getline(file,line)){
                require(line.find('\r')==std::string::npos,"Log contains injected carriage return");
                if(line.find("test.final line1 line2 finish")!=std::string::npos)marker=true;
            }
        }
        require(files>1&&files<=5,"Rotation/retention bound failed");require(marker,"Final marker missing");
        {std::ofstream file(root/"unwritable-parent");file<<"not a directory";}
        diagnostics::start("unwritable-test",root/"unwritable-parent"/"logs");
        diagnostics::write("unwritable.event");Sleep(100);
        require(!diagnostics::flush(200),"Disk failure was not reported");diagnostics::stop();
        std::filesystem::remove_all(root);
        std::cout<<"PASS concurrent producers, rotation, retention, line sanitization, failed writer and shutdown\n";
        return 0;
    }catch(const std::exception& error){diagnostics::stop();std::cerr<<"FAIL "<<error.what()<<" evidence="<<root<<'\n';return 1;}
}
