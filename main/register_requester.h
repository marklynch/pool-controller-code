#ifndef REGISTER_REQUESTER_H
#define REGISTER_REQUESTER_H

// Start the register requester task.
// If no Internet Gateway is detected after a startup delay, this task will
// send CMD 0x39 register read requests for any missing pool state data.
void register_requester_start(void);

// Wake the requester task immediately to check for missing data.
// Safe to call from any task context (e.g. when a light zone is first configured).
void register_requester_notify(void);

#endif // REGISTER_REQUESTER_H
