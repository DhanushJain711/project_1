//
//  scheduler.cpp
//  Processor Scheduler
//
//  Created by ELMOOTAZBELLAH ELNOZAHY on 9/13/26.
//

#include <queue>
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
size_t QueueLength(); // processes waiting in the ready queue
Time_t QueuedWork(); // sum of GetRemaining() over the ready queue, in us

// Policy
void PolicyOnTick(Time_t now); // called at the end of every TimerInterrupt
void OnCoreIdle(CPUId_t c); // called when a core finishes a process and the queue is empty

std::queue<ProcessId_t> readyQ;
ProcessId_t running = InvalidProcessId();


void CreateProcess(ProcessId_t pid) {
    // A new process has been created. Update the scheduler's data structures and decisions accordingly.
    SimOutput("CreateProcess(" + std::to_string(pid) + ")", 4);
    if(running == InvalidProcessId()) {
        running = pid;
        LoadContext(running, 0);
        RunCore(0);
    }
    else {  // There is already a running process
        readyQ.push(pid);
    }
}

void ExitProcess(ProcessId_t pid) {
    // Process finished running. Update the scheduler's data structures and decisions accordingly.
    if(running != pid) {
        ThrowException("A process that was not running is calling exit!!!");
    }
    if(!readyQ.empty()){
        running = readyQ.front();
        readyQ.pop();
        LoadContext(running, 0);
        RunCore(0);
    }
    else {
        running = InvalidProcessId();   // Nothing is running right now
    }
}

void TimerInterrupt(Time_t now) {
    // You received a timer interrupt. This is where you want to execute scheduling decisions
    if(running == InvalidProcessId())       // Nothing to do
        return;
    // Someone was running
    if(readyQ.empty())                      // We have a running process but no other processes are waiting
        return;
    SaveContext(running, 0);
    readyQ.push(running);
    running = readyQ.front();
    readyQ.pop();
    LoadContext(running, 0);
    RunCore(0);
}

void CStateTransitionComplete(CPUId_t core_id){
    
}

void SimulationComplete(Time_t now) {
    // Add any bookkeeping or statistics that you would want to collect. Program terminates after this function returns.
    std::cout << "Run stopped at " << FormatTime(now) << " after consuming " << GetTotalEnergyConsumed()/3600000000.0 << " kWh" << std::endl;
}
