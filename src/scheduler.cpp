//
//  scheduler.cpp
//  Processor Scheduler
//
//  Created by ELMOOTAZBELLAH ELNOZAHY on 9/13/26.
//

#include <queue>
#include <unordered_map>
#include "scheduler.hpp"

enum CoreMode { OFF, NAPPING, WAKING, IDLE, RUNNING };
struct CoreInfo {
    CoreMode mode; //curr mode
    ProcessId_t pid; 
    bool big; // big = cores 0-3, small =  4-7
    PState_t p; // curr pstate
    Time_t idleSince; // last time cor ewas idle
};

extern CoreInfo cores[8]; // total num ocres


// Mechanism
bool Dispatch(CPUId_t c); // start next queued proc on idle core, false if no queue
void Park(CPUId_t c, CState_t cs); // idle -> c4 or c6
void Wake(CPUId_t c); // core -> c1
void SetSpeed(CPUId_t c, PState_t p);
Time_t QueuedWork(); 

void PolicyOnTick(Time_t now); //end of timer interrupt
void OnCoreIdle(CPUId_t c); 

std::deque<ProcessId_t> readyQ;
std::unordered_map<ProcessId_t, CPUId_t> pidCoreMap; //map from pid to core

//init cores
CoreInfo cores[8];
static bool init() {
    //set all cores to idle, no processes on each cor eyet
    for(int i = 0; i < 8; i++) {
        cores[i].mode = IDLE;                    
        cores[i].pid = InvalidProcessId();
        cores[i].big = (i < 4);                  
        cores[i].p = P0;                        
        cores[i].idleSince = 0;
    }
    return true;
}
static bool coresInitialized = init();

bool Dispatch(CPUId_t c) {
    //Check if idle core and waiting process
    if (c >= 8) {
        return false;
    }
    if (cores[c].mode == IDLE && !readyQ.empty()) {
        //get proc
        ProcessId_t pid = readyQ.front();
        readyQ.pop_front();
        //run 
        LoadContext(pid, c);
        RunCore(c);
        //update core
        cores[c].mode = RUNNING;
        cores[c].pid = pid;
        pidCoreMap[pid] = c;
        return true;
    }
    return false;
}

void Park(CPUId_t c, CState_t cs) {
    //set cstate  if core idle
    if ((cores[c].mode == IDLE) && (cs == C4 || cs == C6)) {
        SetCState(c, cs);
        cores[c].mode = ((cs == C4) ? NAPPING : OFF);
    }
}

void Wake(CPUId_t c) {
    //wake  core  if napping or off
    if ((cores[c].mode == NAPPING) || (cores[c].mode == OFF)) {
        SetCState(c, C1);
        cores[c].mode = WAKING;
    }
}

void SetSpeed(CPUId_t c, PState_t p) {
    //update p state
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

//change these to get lowest kwh use
static const Time_t NAP_WAKE_THRESH_US  = 100000;    // wake C4 if drain time exceeds 
static const Time_t OFF_WAKE_THRESH_US  = 3000000;  // wake C6  if drain time exceeds
static const Time_t DEEP_AFTER_US       = 100000;   // C4 -> C6 after this time in idle
static const Time_t ESCALATE_THRESH_US  = 6000000;  // all cores wake and drain above this => speed up
static const int    ESCALATE_TICKS      = 500;      // conseq ticks
static const CPUId_t RESERVE_CORE       = 8;        //napping cores can be demoted to c6


static bool    policyStarted = false;
static bool    wantDeep[8]   = {false};   
static PState_t level        = P3;        // global pstate for all cores
static int     backlogTicks  = 0;


static double CoreSpeed(CPUId_t c) {
   static const double s[5] = {1.0, 0.8, 0.6, 0.4, 0.2};
   return s[cores[c].p] * (cores[c].big ? 1.0 : 0.6);
}


// speed of cores that can b used
static double AwakeCapacity() {
   double cap = 0;
   for (CPUId_t c = 0; c < 8; c++)
       if ((cores[c].mode == RUNNING || cores[c].mode == IDLE || cores[c].mode == WAKING) && !wantDeep[c])
           cap += CoreSpeed(c);
   return cap;
}


static bool AnySleeper() {
   for (CPUId_t c = 0; c < 8; c++)
       if (cores[c].mode == NAPPING || cores[c].mode == OFF) return true;
   return false;
}


// condo 1: small before bigg
//condo 2: napping before off
static int PickSleeper(double drain) {
   const CoreMode modes[2] = {NAPPING, OFF};
   for (int m = 0; m < 2; m++) {
       double thresh = (modes[m] == NAPPING) ? (double)NAP_WAKE_THRESH_US : (double)OFF_WAKE_THRESH_US;
       if (drain <= thresh) continue;
       for (CPUId_t c = 4; c < 8; c++) if (cores[c].mode == modes[m]) return (int)c;
       for (CPUId_t c = 0; c < 4; c++) if (cores[c].mode == modes[m]) return (int)c;
   }
   return -1;
}


static void ApplyLevel() {
   for (CPUId_t c = 0; c < 8; c++) {
        SetSpeed(c, level);
   }

}


void OnCoreIdle(CPUId_t c) {
   Park(c, wantDeep[c] ? C6 : C4);
   wantDeep[c] = false;
}


void PolicyOnTick(Time_t now) {
   if (!policyStarted) {            
       policyStarted = true;
       ApplyLevel();
   }
   for (CPUId_t c = 0; c < 8; c++)
       if (cores[c].mode == RUNNING) wantDeep[c] = false;
   // conod 1 no core idleo n c1 when nothing on q
   if (readyQ.empty())
       for (CPUId_t c = 0; c < 8; c++){
           if (cores[c].mode == IDLE) OnCoreIdle(c);
       }
   // condo 2 wake enough cores to deal w backlock
   double work = (double)QueuedWork();
   double cap  = AwakeCapacity();
   if (work > 0) {
       for (;;) {
           double drain = (cap > 1e-9) ? work / cap : 1e18;  
           int c = PickSleeper(drain);
           if (c < 0) break;
           Wake((CPUId_t)c);                              
           cap += CoreSpeed((CPUId_t)c);                  
       }
   }
   // condo 3 -- increase speed if (1) all cores awake and (2) backlock
   double drain = (cap > 1e-9) ? work / cap : 0;
   if (work > 0 && !AnySleeper() && drain > (double)ESCALATE_THRESH_US) {
       if (++backlogTicks >= ESCALATE_TICKS && level > P0) {
           level = static_cast<PState_t>(level - 1);
           ApplyLevel();
           backlogTicks = 0;
       }
   } else {
       backlogTicks = 0;
       //back to lower speed
       if (work == 0 && level != P3) { 
           level = P3;
           ApplyLevel();
       }
   }
   // napping cores -> c6
   if (readyQ.empty()) {
       for (CPUId_t c = 0; c < 8; c++) {
           if (c == RESERVE_CORE || cores[c].mode != NAPPING || wantDeep[c]) continue;
           if (Now() - cores[c].idleSince > DEEP_AFTER_US) {
               wantDeep[c] = true;
               Wake(c);
           }
       }
   }
}



void CreateProcess(ProcessId_t pid) {
    // A new process has been created. Update the scheduler's data structures and decisions accordingly.
    SimOutput("CreateProcess(" + std::to_string(pid) + ")", 4);
    //Push process to queeu
    readyQ.push_back(pid);
    //Dispatch to idle core 
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
    //clear process from map
    CPUId_t c = pidCoreMap.at(pid);
    pidCoreMap.erase(pid);
    //update core
    cores[c].pid = InvalidProcessId();
    cores[c].mode = IDLE;
    cores[c].idleSince = Now();
    if (!readyQ.empty()) {
        Dispatch(c);
    } else {
        OnCoreIdle(c);
    }
}

void TimerInterrupt(Time_t now) {
    // You received a timer interrupt. This is where you want to execute scheduling decisions
    if(!readyQ.empty()) {
        for (CPUId_t c = 4; c < 8; c++) {
            Dispatch(c);     
        }
        for (CPUId_t c = 0; c < 4; c++) {
            Dispatch(c);
        }
    }
    //init policy
    PolicyOnTick(now);
}

void CStateTransitionComplete(CPUId_t core_id){
    //mark core idle
    if (cores[core_id].mode == WAKING) {
        cores[core_id].mode = IDLE;
        cores[core_id].idleSince = Now();
        if (!readyQ.empty()) {
            //dispacth idle core if proc waiting
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
