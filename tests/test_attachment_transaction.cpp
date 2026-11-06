#include <cstdio>
#include <initializer_list>
#include "game/attachment_transaction.h"
using namespace holevr::game::custom::transaction;
static int errors=0;
#define CHECK(x) do{if(!(x)){std::printf("FAIL %d: %s\n",__LINE__,#x);++errors;}}while(0)
int main(){
    // Before metadata replacement, during rebuilding,
    // and after failed verification of the installed part/ammunition.
    for(int failure=-1;failure<4;++failure){
        int saved=10,live=10,undos=0;bool entered=false;
        const auto result=Run(
            [&]{if(failure==0)return false;saved=20;return true;},
            [&]{entered=true;if(failure==1)return false;live=20;return failure!=2 && failure!=3;},
            [&]{++undos;live=10;return true;},
            [&]{++undos;saved=10;return true;});
        if(failure<0){CHECK(result==Result::kApplied);CHECK(saved==20&&live==20&&undos==0);}
        else if(failure==0){CHECK(result==Result::kSaveRefused);CHECK(!entered&&saved==10&&live==10&&undos==0);}
        else {CHECK(result==Result::kRestored);CHECK(saved==10&&live==10&&undos==2);}
    }
    for(bool live_fails:{false,true}){
        bool tried_live=false,tried_save=false;
        const auto r=Run([]{return true;},[]{return false;},[&]{tried_live=true;return !live_fails;},
                         [&]{tried_save=true;return live_fails;});
        CHECK(r==Result::kRestoreFailed);CHECK(tried_live&&tried_save);
    }
    std::printf("attachment transaction: %d failures\n",errors);return errors?1:0;
}
