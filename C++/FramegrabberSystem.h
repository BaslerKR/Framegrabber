#ifndef FRAMEGRABBERSYSTEM_H
#define FRAMEGRABBERSYSTEM_H

#include "Framegrabber.h"
#include "basler_fg.h"
#include "sisoboards.h"

using namespace std;
inline void syslog(string message, bool warning=false)
{
    if(!warning) cout << "[Framegrabber] " << message << endl;
    else cerr << "[Framegrabber] " << message << endl;
}

class FramegrabberSystem
{
public:
    ~FramegrabberSystem(){
        for (auto &item : _grabberList) {
            delete item.first;
        }
        _grabberList.clear();
        Fg_FreeLibraries();
    }

    bool init(){
        char buffer[256] = "";
        unsigned int buflen = 256;
        buffer[0] = 0;
        if(Fg_getSystemInformation(nullptr, INFO_NR_OF_BOARDS, PROP_ID_VALUE, 0, buffer, &buflen) == FG_OK){
            _boardCnt = atoi(buffer);
        }
        for(int i = 0; i < _boardCnt; ++i){
            auto name = Fg_getBoardNameByType(Fg_getBoardType(i), 0);

            std::string serial, lane, gen, payload;
            Fg_getStringSystemInformationForBoardIndex(i, Fg_Info_Selector::INFO_BOARDSERIALNO, FgProperty::PROP_ID_VALUE, serial);
            Fg_getStringSystemInformationForBoardIndex(i, Fg_Info_Selector::INFO_STATUS_PCI_LINK_WIDTH, FgProperty::PROP_ID_VALUE, lane);
            Fg_getStringSystemInformationForBoardIndex(i, Fg_Info_Selector::INFO_STATUS_PCI_LINK_SPEED, FgProperty::PROP_ID_VALUE, gen);
            Fg_getStringSystemInformationForBoardIndex(i, Fg_Info_Selector::INFO_STATUS_PCI_PAYLOAD_SIZE, FgProperty::PROP_ID_VALUE, payload);

            std::string firmware, driver, hardware, boardName;
            Fg_getStringSystemInformationForBoardIndex(i, Fg_Info_Selector::INFO_FIRMWAREVERSION, FgProperty::PROP_ID_VALUE, firmware);
            Fg_getStringSystemInformationForBoardIndex(i, Fg_Info_Selector::INFO_HARDWAREVERSION, FgProperty::PROP_ID_VALUE, hardware);
            Fg_getStringSystemInformationForBoardIndex(i, Fg_Info_Selector::INFO_DRIVERVERSION, FgProperty::PROP_ID_VALUE, driver);

            syslog(std::string(name) + " (" + serial + ") (" + std::to_string(i) + ") detected.");
            syslog(std::string("-- ") + "PCIe x" + lane + " Gen" + gen + " Payload " + payload + " bytes.");
            syslog(std::string("-- ") + "Firmware version: " + firmware);
            syslog(std::string("-- ") + "Driver version: " + driver);
            syslog(std::string("-- ") + "Hardware version: " + hardware);

            _grabberList.push_back(std::pair(new Framegrabber(serial, i), i));
        }
        if(FG_OK == Fg_InitLibraries(nullptr)){
            syslog("Library initialization succeded.");
            _init = true;
            return true;
        }else{
            syslog("Library initialization failed.", true);
            _init = false;
            return false;
        }
    }

    Framegrabber *addFramegrabber(unsigned int boardIndex=0){
        return getFramegrabberFromIndex(boardIndex);
    }
    Framegrabber *getFramegrabberFromIndex(unsigned int boardIndex=0){
        syslog("Searching for the grabber at index " + std::to_string(boardIndex) + ".");
        for(const auto cur : _grabberList){
            if(cur.second == boardIndex){
                syslog("Matched grabber ("+ cur.first->getSerialNumber() +") found. ");
                return cur.first;
            }
        }
        syslog("Failed to find a matched grabber.", true);
        return nullptr;
    }
    void removeFramegrabber(unsigned int boardIndex=0);
    unsigned int getBoardCount(){
        return _boardCnt;
    }

    bool isInitialized(){ return _init; }

private:
    // Framegrabber and board index
    std::vector<std::pair<Framegrabber*, unsigned int>> _grabberList;
    unsigned int _boardCnt=0;
    bool _init = false;
};

#endif // FRAMEGRABBERSYSTEM_H

