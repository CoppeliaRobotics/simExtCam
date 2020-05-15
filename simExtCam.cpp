#include <windows.h>
#include "escapi.h"
#include "Mmsystem.h"
#include <shlwapi.h> // for the "PathRemoveFileSpec" function
#include <vector>
#include <iostream>
#include "simLib.h"
#include "scriptFunctionData.h"

#pragma comment(lib, "Winmm.lib")
#pragma comment(lib, "Shlwapi.lib")

#ifdef _MANAGED
#pragma managed(push, off)
#endif

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    return TRUE;
}

#ifdef _MANAGED
#pragma managed(pop)
#endif

#define SIM_DLLEXPORT extern "C" __declspec(dllexport)

#define CONCAT(x,y,z) x y z
#define strConCat(x,y,z)    CONCAT(x,y,z)

// Following few for backward compatibility:
#define LUA_START_OLD "simExtCamStart"
#define LUA_END_OLD "simExtCamEnd"
#define LUA_INFO_OLD "simExtCamInfo"
#define LUA_GRAB_OLD "simExtCamGrab"

int startCountOverall=0;
int deviceCount=0;
int startCountPerDevice[4]={0,0,0,0}; // first 4 are for devices 0-3
volatile bool openCaptureDevices[4]={false,false,false,false}; // The capture devices (out of a max of 4) that have been initialized
volatile bool _camThreadLaunched=false;
struct SimpleCapParams captureInfo[4];
CRITICAL_SECTION m_cs;
bool displayAcknowledgment=false;

LIBRARY simLib;

void killThread()
{
    _camThreadLaunched=false;
    while (!_camThreadLaunched)
        Sleep(2);
    _camThreadLaunched=false;
}

DWORD WINAPI _camThread(LPVOID lpParam)
{
    _camThreadLaunched=true;
    while (_camThreadLaunched)
    {
        static bool firstHere=true;
        if (firstHere)
            displayAcknowledgment=true;
        firstHere=false;
        EnterCriticalSection(&m_cs);
        for (int i=0;i<4;i++)
        {
            if (openCaptureDevices[i])
            {
                if (isCaptureDone(i))
                    doCapture(i);
            }
        }
        LeaveCriticalSection(&m_cs);
        Sleep(10);
    }
    for (int i=0;i<4;i++)
    {
        if (openCaptureDevices[i])
        {
            deinitCapture(i);
            delete[] captureInfo[i].mTargetBuf;
        }
        openCaptureDevices[i]=false;
    }
    _camThreadLaunched=true;
    return(0);
}


#define LUA_START "simCam.start"
const int inArgs_START[] = {
	3,
	sim_script_arg_int32,0,
	sim_script_arg_int32,0,
	sim_script_arg_int32,0,
};

void LUA_START_CALLBACK(SScriptCallBack* cb)
{ // the callback function of the new Lua command
	CScriptFunctionData D;
	int result = -1; // error
	int returnResolution[2] = { 0,0 };

	if (D.readDataFromStack(cb->stackID, inArgs_START, inArgs_START[0], LUA_START))
	{
		std::vector<CScriptFunctionDataItem>* inData = D.getInDataPtr();
		int arg1 = inData->at(0).int32Data[0];
		int arg2 = inData->at(1).int32Data[0];
		int arg3 = inData->at(2).int32Data[0];
		if ( (countCaptureDevices()>arg1)&&(arg1 >=0)&&(arg1<4) )
        {
            if (!openCaptureDevices[arg1])
            { // We can set the new resolution
                bool goOn=true;
                if (startCountOverall==0)
                { // Launch the thread!
                    _camThreadLaunched=false;
                    CreateThread(NULL,0,_camThread,NULL,THREAD_PRIORITY_NORMAL,NULL);
                    while (!_camThreadLaunched)
                        Sleep(2);
                    if (deviceCount<1)
                    {
                        simSetLastError(LUA_START,"ESCAPI initialization failure or no devices found."); // output an error
                        killThread();
                        goOn=false;
                    }
                }

                if (goOn)
                {
                    captureInfo[arg1].mWidth=arg2;
                    captureInfo[arg1].mHeight= arg3;
                    captureInfo[arg1].mTargetBuf=new int[arg2*arg3];
                    if (initCapture(arg1,&captureInfo[arg1])!=0)
                    {
                        doCapture(arg1);
                        openCaptureDevices[arg1]=true;
                        returnResolution[0]= arg2;
                        returnResolution[1]= arg3;
                        result=1; // success!
                        startCountOverall++;
                        startCountPerDevice[arg1]++;
                    }
                    else
                    {
                        delete[] captureInfo[arg1].mTargetBuf;
                        simSetLastError(LUA_START,"Device may already be in use."); // output an error
                    }
                }
            }
            else
            { // We have to retrieve the current resolution
                returnResolution[0]=captureInfo[arg1].mWidth;
                returnResolution[1]=captureInfo[arg1].mHeight;
                result=0;
                startCountOverall++;
                startCountPerDevice[arg1]++;
            }
        }
        else
            simSetLastError(LUA_START,"Invalid device index."); // output an error
    }

	D.pushOutData(CScriptFunctionDataItem(result));
	if (result > -1)
	{
		D.pushOutData(CScriptFunctionDataItem(returnResolution[0]));
		D.pushOutData(CScriptFunctionDataItem(returnResolution[1]));
	}
	D.writeDataToStack(cb->stackID);
}


#define LUA_END "simCam.stop"
const int inArgs_END[] = {
	1,
	sim_script_arg_int32,0,
};

void LUA_END_CALLBACK(SScriptCallBack* cb)
{ // the callback function of the new Lua command
	CScriptFunctionData D;
	int result = -1; // error

	if (D.readDataFromStack(cb->stackID, inArgs_END, inArgs_END[0], LUA_END))
	{
		std::vector<CScriptFunctionDataItem>* inData = D.getInDataPtr();
		int arg1 = inData->at(0).int32Data[0];
        if ( (arg1<4)&&(startCountPerDevice[arg1]>0) )
        {
            startCountOverall--;
            startCountPerDevice[arg1]--;
            if (startCountPerDevice[arg1]==0)
            {
                EnterCriticalSection(&m_cs);
                deinitCapture(arg1);
                delete[] captureInfo[arg1].mTargetBuf;
                openCaptureDevices[arg1]=false;
                LeaveCriticalSection(&m_cs);
            }
            if (startCountOverall==0)
                killThread();
            result=1;
        }
        else
            simSetLastError(LUA_END,"Invalid device index."); // output an error
    }

	D.pushOutData(CScriptFunctionDataItem(result));
	D.writeDataToStack(cb->stackID);
}

#define LUA_INFO "simCam.info"
const int inArgs_INFO[] = {
	1,
	sim_script_arg_int32,0,
};

void LUA_INFO_CALLBACK(SScriptCallBack* cb)
{ // the callback function of the new Lua command
	CScriptFunctionData D;
	char infoString[200];
	infoString[0] = 0;

	if (D.readDataFromStack(cb->stackID, inArgs_INFO, inArgs_INFO[0], LUA_INFO))
	{
		std::vector<CScriptFunctionDataItem>* inData = D.getInDataPtr();
		int arg1 = inData->at(0).int32Data[0];

        if ( (countCaptureDevices()>arg1)&&(arg1 >=0)&&(arg1<4) )
        {
            getCaptureDeviceName(arg1,infoString,200);
        }
        else
            simSetLastError(LUA_INFO,"Wrong index."); // output an error
    }
	int l = int(strlen(infoString));
	if (l != 0)
		D.pushOutData(CScriptFunctionDataItem(infoString));
	D.writeDataToStack(cb->stackID);
}

#define LUA_GRAB "simCam.grab"
const int inArgs_GRAB[] = {
	2,
	sim_script_arg_int32,0,
	sim_script_arg_int32,0,
};

void LUA_GRAB_CALLBACK(SScriptCallBack* cb)
{ // the callback function of the new Lua command
	CScriptFunctionData D;
	int retVal = 0; // Means error

	if (D.readDataFromStack(cb->stackID, inArgs_GRAB, inArgs_GRAB[0], LUA_GRAB))
	{
		std::vector<CScriptFunctionDataItem>* inData = D.getInDataPtr();
		int arg1 = inData->at(0).int32Data[0];
		int arg2 = inData->at(1).int32Data[0];
        if ( (countCaptureDevices()>arg1)&&(arg1>=0)&&(arg1<4) )
        {
            if (startCountPerDevice[arg1]>0)
            {
                if (openCaptureDevices[arg1])
                {
                    int errorModeSaved;
                    simGetIntegerParameter(sim_intparam_error_report_mode,&errorModeSaved);
                    simSetIntegerParameter(sim_intparam_error_report_mode,sim_api_errormessage_ignore);
                    int t=simGetObjectType(arg2);
                    simSetIntegerParameter(sim_intparam_error_report_mode,errorModeSaved); // restore previous settings
                    if (t==sim_object_visionsensor_type)
                    {
                        int r[2]={0,0};
                        simGetVisionSensorResolution(arg2,r);
                        if ( (r[0]==captureInfo[arg1].mWidth)&&(r[1]==captureInfo[arg1].mHeight) )
                        {
                            float* buff=new float[r[0]*r[1]*3];

                            for (int i=0;i<r[1];i++)
                            {
                                int y0=r[0]*i;
                                int y1=r[0]*(r[1]-i-1);
                                for (int j=0;j<r[0];j++)
                                { // Info is provided as BGR!! (and not RGB)
                                    buff[3*(y0+j)+0]=float(((BYTE*)captureInfo[arg1].mTargetBuf)[4*(y1+j)+2])/255.0f;
                                    buff[3*(y0+j)+1]=float(((BYTE*)captureInfo[arg1].mTargetBuf)[4*(y1+j)+1])/255.0f;
                                    buff[3*(y0+j)+2]=float(((BYTE*)captureInfo[arg1].mTargetBuf)[4*(y1+j)+0])/255.0f;
                                }
                            }
                            simSetVisionSensorImage(arg2,buff);
                            delete[] buff;
                            retVal=1;                               
                        }
                        else
                            simSetLastError(LUA_GRAB,"Resolutions do not match."); // output an error
                    }
                    else
                        simSetLastError(LUA_GRAB,"Invalid vision sensor handle."); // output an error
                }
                else
                    simSetLastError(LUA_GRAB,"Resolution was not set."); // output an error
            }
            else
                simSetLastError(LUA_GRAB,"simExtCamStart was not called."); // output an error
        }
        else
            simSetLastError(LUA_GRAB,"Wrong index."); // output an error
    }

	D.pushOutData(CScriptFunctionDataItem(retVal));
	D.writeDataToStack(cb->stackID);
}


SIM_DLLEXPORT unsigned char simStart(void* reservedPointer,int reservedInt)
{ // This is called just once, at the start of CoppeliaSim

    // Dynamically load and bind CoppeliaSim functions:
    char curDirAndFile[1024];
    GetModuleFileName(NULL,curDirAndFile,1023);
    PathRemoveFileSpec(curDirAndFile);
    std::string currentDirAndPath(curDirAndFile);
    std::string temp(currentDirAndPath);
    temp+="\\coppeliaSim.dll";
    simLib=loadSimLibrary(temp.c_str());
    if (simLib==NULL)
    {
        printf("simExtCam: error: could not find or correctly load the CoppeliaSim library. Cannot start the plugin.\n"); // cannot use simAddLog here.
        return(0); // Means error, CoppeliaSim will unload this plugin
    }
    if (getSimProcAddresses(simLib)==0)
    {
        printf("simExtCam: error: could not find all required functions in the CoppeliaSim library. Cannot start the plugin.\n"); // cannot use simAddLog here.
        unloadSimLibrary(simLib);
        return(0); // Means error, CoppeliaSim will unload this plugin
    }

    // Marc modified following function to return a neg. value in case of initialization error:
    deviceCount=setupESCAPI();
    if (deviceCount<0)
    {
        std::string txt("ESCAPI initialization failed (error code: ");
        txt+=std::to_string(deviceCount);
        txt+=". Is 'escapi.dll' available? Cannot start the plugin.";
        simAddLog("Cam",sim_verbosity_errors,txt.c_str());
        unloadSimLibrary(simLib);
        return(0); // initialization failed!!
    }

	simRegisterScriptVariable("simCam", "require('simExtCam')", 0);

	// Register the new functions:
	simRegisterScriptCallbackFunction(strConCat(LUA_START, "@", "Cam"), strConCat("number result,number resX,number resY=", LUA_START, "(number deviceIndex,number resX,number resY)"), LUA_START_CALLBACK);
	simRegisterScriptCallbackFunction(strConCat(LUA_END, "@", "Cam"), strConCat("number result=", LUA_END, "(number deviceIndex)"), LUA_END_CALLBACK);
	simRegisterScriptCallbackFunction(strConCat(LUA_INFO, "@", "Cam"), strConCat("string info=", LUA_INFO, "(number deviceIndex)"), LUA_INFO_CALLBACK);
	simRegisterScriptCallbackFunction(strConCat(LUA_GRAB, "@", "Cam"), strConCat("number result=", LUA_GRAB, "(number deviceIndex,number visionSensorHandle)"), LUA_GRAB_CALLBACK);

	// Following for backward compatibility:
	simRegisterScriptVariable(LUA_START_OLD, LUA_START, -1);
	simRegisterScriptCallbackFunction(strConCat(LUA_START_OLD, "@", "Cam"), strConCat("Please use the ", LUA_START, " notation instead"), 0);
	simRegisterScriptVariable(LUA_END_OLD, LUA_END, -1);
	simRegisterScriptCallbackFunction(strConCat(LUA_END_OLD, "@", "Cam"), strConCat("Please use the ", LUA_END, " notation instead"), 0); 
	simRegisterScriptVariable(LUA_INFO_OLD, LUA_INFO, -1);
	simRegisterScriptCallbackFunction(strConCat(LUA_INFO_OLD, "@", "Cam"), strConCat("Please use the ", LUA_INFO, " notation instead"), 0);
	simRegisterScriptVariable(LUA_GRAB_OLD, LUA_GRAB, -1);
	simRegisterScriptCallbackFunction(strConCat(LUA_GRAB_OLD, "@", "Cam"), strConCat("Please use the ", LUA_GRAB, " notation instead"), 0);

    InitializeCriticalSection(&m_cs);

    return(4); // initialization went fine, return the version number of this extension module (can be queried with simGetModuleName)
    // version 1 was for CoppeliaSim versions before CoppeliaSim 2.5.12
    // version 2 was for CoppeliaSim versions before CoppeliaSim 2.6.0
	// version 3 was for CoppeliaSim versions before CoppeliaSim 3.4.1
}

SIM_DLLEXPORT void simEnd()
{ // This is called just once, at the end of CoppeliaSim
    // Release resources here..

    // The thread should have been exited alreads (all simulations stopped!)

    unloadSimLibrary(simLib); // release the library
}

SIM_DLLEXPORT void* simMessage(int message,int* auxiliaryData,void* customData,int* replyData)
{ // This is called quite often. Just watch out for messages/events you want to handle
    // This function should not generate any error messages:
    int errorModeSaved;
    simGetIntegerParameter(sim_intparam_error_report_mode,&errorModeSaved);
    simSetIntegerParameter(sim_intparam_error_report_mode,sim_api_errormessage_ignore);

    void* retVal=NULL;

    // Acknowledgment message display:
    // *****************************************************************
    static int auxConsoleHandleForAcknowledgmentDisplay=-1;
    static int acknowledgmentDisplayStartTime=0;
    if (displayAcknowledgment)
    {
        auxConsoleHandleForAcknowledgmentDisplay=simAuxiliaryConsoleOpen("Acknowledgments",10,2+4+16,NULL,NULL,NULL,NULL);
        simAuxiliaryConsolePrint(auxConsoleHandleForAcknowledgmentDisplay,"\nThe cam plugin contains the ESCAPI library, which is courtesy of Jari Komppa.");
        acknowledgmentDisplayStartTime=timeGetTime();
        displayAcknowledgment=false;
    }
    if ( (auxConsoleHandleForAcknowledgmentDisplay!=-1)&&(timeGetTime()-acknowledgmentDisplayStartTime>5000) )
    {
        simAuxiliaryConsoleClose(auxConsoleHandleForAcknowledgmentDisplay);
        auxConsoleHandleForAcknowledgmentDisplay=-1;
    }
    // *****************************************************************

    // Clean-up at simulation end:
    // *****************************************************************
    if (message==sim_message_eventcallback_simulationended)
    {
        if (auxConsoleHandleForAcknowledgmentDisplay!=-1)
        {
            simAuxiliaryConsoleClose(auxConsoleHandleForAcknowledgmentDisplay);
            auxConsoleHandleForAcknowledgmentDisplay=-1;
        }
        for (int i=0;i<4;i++) // for the 4 devices
        {
            while (startCountPerDevice[i]>0)
            {
                startCountOverall--;
                startCountPerDevice[i]--;
                if (startCountOverall==0)
                    killThread();
            }
        }
    }
    // *****************************************************************

    simSetIntegerParameter(sim_intparam_error_report_mode,errorModeSaved); // restore previous settings
    return(retVal);
}
