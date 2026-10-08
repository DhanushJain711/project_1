//
//  scheduler.cpp
//  Processor Scheduler
//
//  Created by ELMOOTAZBELLAH ELNOZAHY on 9/13/26.
//

#include <deque>
#include <unordered_map>
#include "scheduler.hpp"

enum CoreMode { OFF, NAPPING, WAKING, IDLE, RUNNING };
struct CoreInfo {
    CoreMode mode; // what the core is doing right now
    ProcessId_t pid; // process on the core, InvalidProcessId() if none
    bool big; // true for cores 0-3, false for 4-7
    PState_t p; // current P-state
    Time_t idleSince; // when the core last became IDLE (for sleep thresholds)
};

extern CoreInfo cores[8];


// Mechanism
bool Dispatch(CPUId_t c); // start the next queued process on an IDLE core; false if queue empty
void Park(CPUId_t c, CState_t cs); // put an IDLE core into C4 (NAPPING) or C6 (OFF)
void Wake(CPUId_t c); // start waking a NAPPING/OFF core back to C1 (becomes WAKING)
void SetSpeed(CPUId_t c, PState_t p);
Time_t QueuedWork(); // sum of GetRemaining() over the ready queue, in us

// Policy
void PolicyOnTick(Time_t now); // called at the end of every TimerInterrupt
void OnCoreIdle(CPUId_t c); // called when a core finishes a process and the queue is empty

std::deque<ProcessId_t> readyQ;
std::unordered_map<ProcessId_t, CPUId_t> pidCoreMap; //map from pid to core

//Initialize cores
CoreInfo cores[8];
static bool InitializeCores() {
    for(int i = 0; i < 8; i++) {
        cores[i].mode = IDLE;                    
        cores[i].pid = InvalidProcessId();
        cores[i].big = (i < 4);                  
        cores[i].p = P0;                        
        cores[i].idleSince = 0;
    }
    return true;
}
static bool coresInitialized = InitializeCores();

bool Dispatch(CPUId_t c) {
    //Check if idle core and waiting process
    if (c >= 8) {
        return false;
    }
    if (cores[c].mode == IDLE && !readyQ.empty()) {
        //Get process from queue
        ProcessId_t pid = readyQ.front();
        readyQ.pop_front();
        //Run process
        LoadContext(pid, c);
        RunCore(c);
        //Update core table and map
        cores[c].mode = RUNNING;
        cores[c].pid = pid;
        pidCoreMap[pid] = c;
        return true;
    }
    return false;
}

void Park(CPUId_t c, CState_t cs) {
    //Set cstate only if core is idle
    if (cores[c].mode == IDLE && (cs == C4 || cs == C6)) {
        SetCState(c, cs);
        cores[c].mode = ((cs == C4) ? NAPPING : OFF);
    }
}

void Wake(CPUId_t c) {
    //Wake up core only if napping or off
    if (cores[c].mode == NAPPING || cores[c].mode == OFF) {
        SetCState(c, C1);
        cores[c].mode = WAKING;
    }
}

void SetSpeed(CPUId_t c, PState_t p) {
    //Update p state
    SetPState(c, p);
    cores[c].p = p;
}

Time_t QueuedWork() {
    //sum of remaining time of processes on queue
    Time_t time = 0;
    for (ProcessId_t pid: readyQ) {
        time += GetRemaining(pid);
    }
    return time;
}

//TEMPORARY POLICY: replace in part B
//======
void OnCoreIdle(CPUId_t c) {
    Park(c, C6);
}

void PolicyOnTick(Time_t now) {
    if(QueuedWork() == 0)
        return;
    // Wake one sleeping core if work is waiting and none is already waking
    for(CPUId_t c = 0; c < 8; c++)
        if(cores[c].mode == WAKING)
            return;
    for(CPUId_t c = 4; c < 8; c++) {
        if(cores[c].mode == OFF || cores[c].mode == NAPPING) {
            Wake(c);
            return;
        }
    }
    for(CPUId_t c = 0; c < 4; c++) {
        if(cores[c].mode == OFF || cores[c].mode == NAPPING) {
            Wake(c);
            return;
        }
    }
}
//======

void CreateProcess(ProcessId_t pid) {
    // A new process has been created. Update the scheduler's data structures and decisions accordingly.
    SimOutput("CreateProcess(" + std::to_string(pid) + ")", 4);
    //Push process onto ready queue
    readyQ.push_back(pid);
    //Dispatch to idle core if exists; start with small cores, then big cores
    for (CPUId_t c = 4; c < 8; c++) {
        Dispatch(c);
    }
    for (CPUId_t c = 0; c < 4; c++) {
        Dispatch(c);
    }
}

void ExitProcess(ProcessId_t pid) {
    // Process finished running. Update the scheduler's data structures and decisions accordingly.
    if(pidCoreMap.find(pid) == pidCoreMap.end()) {
        ThrowException("A process that was not running is calling exit!!!");
    }
    //Clear process from map
    CPUId_t c = pidCoreMap.at(pid);
    pidCoreMap.erase(pid);
    //Update core
    cores[c].pid = InvalidProcessId();
    cores[c].mode = IDLE;
    cores[c].idleSince = Now();
    if (!readyQ.empty()) {
        //Dispatch idle core if processes waiting
        Dispatch(c);
    } else {
        OnCoreIdle(c);
    }
}

void TimerInterrupt(Time_t now) {
    // You received a timer interrupt. This is where you want to execute scheduling decisions
    // Dispatch waiting processes to idle cores
    if(!readyQ.empty()) {
        for (CPUId_t c = 4; c < 8; c++) {
            Dispatch(c);     
        }
        for (CPUId_t c = 0; c < 4; c++) {
            Dispatch(c);
        }
    }
    //Initiate policy
    PolicyOnTick(now);
}

void CStateTransitionComplete(CPUId_t core_id){
    //Mark core idle once finished waking
    if (cores[core_id].mode == WAKING) {
        cores[core_id].mode = IDLE;
        cores[core_id].idleSince = Now();
        if (!readyQ.empty()) {
            //Dispatch idle core if processes waiting
            Dispatch(core_id);
        } else {
            OnCoreIdle(core_id);
        }
    }
}

void SimulationComplete(Time_t now) {
    // Add any bookkeeping or statistics that you would want to collect. Program terminates after this function returns.
    std::cout << "Run stopped at " << FormatTime(now) << " after consuming " << GetTotalEnergyConsumed()/3600000000.0 << " kWh" << std::endl;
}
