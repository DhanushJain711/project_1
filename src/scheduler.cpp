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



// ===== POLICY (B) =====
// Suhani: energy policy. Reads cores[]/readyQ, acts only through Park/Wake/SetSpeed.
//
// Why (numbers from the work-split tables):
//  * Idle in C1 is expensive (9.6 big / 4.8 small) -> never leave a core idle; nap it in C4 (1.6 / 0.8).
//  * Energy per unit work is lowest at P3 and lowest on small cores
//    (small P3 11.7 < big P3 14 < small P2 15 < big P2 18 < small P0 18.3),
//    so we wake MORE cores at P3 before raising any P-state. P4 is never used.
//  * Waking costs time but no energy (core keeps drawing its sleep power), so C6 (0 W) is
//    free to use; the only cost is 2 s latency. We therefore wake a C6 core only if the backlog
//    would take longer than that latency to drain.
//  * Decisions use observed state (queued work, core modes), never absolute clock times.


// ---- tunables (sweep these for the report) ----
static const Time_t NAP_WAKE_THRESH_US  = 100000;    // wake a C4 core if drain time exceeds this
static const Time_t OFF_WAKE_THRESH_US  = 3000000;  // wake a C6 core if drain time exceeds this (~its wake latency)
static const Time_t DEEP_AFTER_US       = 100000;   // C4 -> C6 after this long idle
static const Time_t ESCALATE_THRESH_US  = 6000000;  // all cores awake and drain above this => speed up
static const int    ESCALATE_TICKS      = 500;      // ...for this many consecutive ticks
static const CPUId_t RESERVE_CORE       = 99;        // small core that is never demoted to C6


static bool    policyStarted = false;
static bool    wantDeep[8]   = {false};   // core is being woken only so it can be re-parked in C6
static PState_t level        = P3;        // global P-state used by every core
static int     backlogTicks  = 0;


static double CoreSpeed(CPUId_t c) {
   static const double s[5] = {1.0, 0.8, 0.6, 0.4, 0.2};
   return s[cores[c].p] * (cores[c].big ? 1.0 : 0.6);
}


// speed of cores that are (or will soon be) available for real work
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


// Best sleeping core to wake given the current drain estimate, or -1.
// Order: small before big (more efficient), napping (fast wake) before off.
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
   for (CPUId_t c = 0; c < 8; c++) SetSpeed(c, level);
}


// Called when a core is IDLE and nothing is queued: nap it (or sleep deeply if it was woken to do so).
void OnCoreIdle(CPUId_t c) {
   Park(c, wantDeep[c] ? C6 : C4);
   wantDeep[c] = false;
}


void PolicyOnTick(Time_t now) {
   if (!policyStarted) {            // startup: everything to P3
       policyStarted = true;
       ApplyLevel();
   }
   for (CPUId_t c = 0; c < 8; c++)
       if (cores[c].mode == RUNNING) wantDeep[c] = false;


   // 1. Never leave a core idling in C1 while there is nothing to run.
   if (readyQ.empty())
       for (CPUId_t c = 0; c < 8; c++)
           if (cores[c].mode == IDLE) OnCoreIdle(c);


   // 2. Capacity controller: wake enough cores that the backlog drains quickly.
   double work = (double)QueuedWork();
   double cap  = AwakeCapacity();
   if (work > 0) {
       for (;;) {
           double drain = (cap > 1e-9) ? work / cap : 1e18;   // no awake core => must wake one
           int c = PickSleeper(drain);
           if (c < 0) break;
           Wake((CPUId_t)c);                                  // counts as capacity immediately,
           cap += CoreSpeed((CPUId_t)c);                      // so one backlog never over-wakes
       }
   }


   // 3. Speed escalation: only when every core is already awake and the backlog persists.
   double drain = (cap > 1e-9) ? work / cap : 0;
   if (work > 0 && !AnySleeper() && drain > (double)ESCALATE_THRESH_US) {
       if (++backlogTicks >= ESCALATE_TICKS && level > P0) {
           level = static_cast<PState_t>(level - 1);
           ApplyLevel();
           backlogTicks = 0;
       }
   } else {
       backlogTicks = 0;
       if (work == 0 && level != P3) {    // backlog cleared: back to the efficient speed
           level = P3;
           ApplyLevel();
       }
   }


   // 4. Demote cores that have napped a long time with no work to C6 (0 W).
   //    Mechanism only parks IDLE cores, so wake (free) then re-park in C6 on completion.
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
// ===== END POLICY (B) =====


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
