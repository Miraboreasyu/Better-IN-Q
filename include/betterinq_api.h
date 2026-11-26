#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BETTERINQ_API_VERSION_1 1u

typedef uint64_t BetterINQ_Session;

typedef enum BetterINQ_Result {
    BETTERINQ_OK                   = 0,
    BETTERINQ_PENDING              = 1,
    BETTERINQ_ERR_DISABLED         = -1,
    BETTERINQ_ERR_NOT_WHITELISTED  = -2,
    BETTERINQ_ERR_BAD_VERSION      = -3,
    BETTERINQ_ERR_INVALID_SESSION  = -4,
    BETTERINQ_ERR_INVALID_ARG      = -5,
    BETTERINQ_ERR_NOT_READY        = -6,
    BETTERINQ_ERR_DORMANT          = -7,
    BETTERINQ_ERR_NOT_LINKED       = -8,
    BETTERINQ_ERR_BUSY             = -9,
    BETTERINQ_ERR_UNSAFE_STAGE     = -10,
    BETTERINQ_ERR_QUEUE_FULL       = -11,
    BETTERINQ_ERR_RATE_LIMITED     = -12,
    BETTERINQ_ERR_NOT_FOUND        = -13,
    BETTERINQ_ERR_IO_FAILED        = -14
} BetterINQ_Result;

typedef enum BetterINQ_CommandState {
    BETTERINQ_CMD_UNKNOWN   = 0,
    BETTERINQ_CMD_QUEUED    = 1,
    BETTERINQ_CMD_COMPLETED = 2,
    BETTERINQ_CMD_FAILED    = 3
} BetterINQ_CommandState;

enum {
    BETTERINQ_RESTART_OVERRIDE_QUEST       = 1u << 0,
    BETTERINQ_RESTART_OVERRIDE_SET         = 1u << 1,
    BETTERINQ_RESTART_OVERRIDE_RESET       = 1u << 2,
    BETTERINQ_RESTART_OVERRIDE_ACCEPT_ONLY = 1u << 3,
    BETTERINQ_RESTART_OVERRIDE_WYVERN      = 1u << 4,

    BETTERINQ_LOADOUT_ALLOW_IN_QUEST       = 1u << 0,
    BETTERINQ_LOADOUT_ITEM_IS_STORE        = 1u << 1,
    BETTERINQ_LOADOUT_LINK_RADIAL          = 1u << 2,
    BETTERINQ_LOADOUT_SET_LINK_RADIAL      = 1u << 3,

    BETTERINQ_PATCH_ACTIVE_SET             = 1u << 0,
    BETTERINQ_PATCH_QUEST_ID               = 1u << 1,
    BETTERINQ_PATCH_FOOD                   = 1u << 2,
    BETTERINQ_PATCH_RESET                  = 1u << 3,
    BETTERINQ_PATCH_ACCEPT_ONLY            = 1u << 4,
    BETTERINQ_PATCH_WYVERN                 = 1u << 5
};

typedef struct BetterINQ_Status {
    uint32_t structSize;
    uint32_t apiVersion;
    uint8_t  ready;
    uint8_t  enabled;
    uint8_t  characterLinked;
    uint8_t  startupReady;
    uint8_t  loading;                   // 0=idle, 1=loading, 2=unknown
    uint8_t  hasUnsavedExternalChanges; // 1 = session overrides active, not saved to INI
    uint8_t  reserved[2];
    int32_t  saveSlot;                  // 1..3
    uint32_t stage;                     // game stage (1=hub, 2=quest, 4/5=post, 0xD=expedition)
    int32_t  restartStage;              // BetterINQ state machine (0..4)
    int32_t  activeSet;                 // effective active [Set.id], -1=none
    int32_t  questId;                   // effective questId, 0=last
    int32_t  food;                      // effective global meal id, -1=none
    int32_t  enteredQuest;              // last quest entered in session
    uint8_t  reset;
    uint8_t  acceptOnly;
    uint8_t  wyvern;
    uint8_t  reserved2;
} BetterINQ_Status;

typedef struct BetterINQ_SetInfo {
    uint32_t structSize;
    int32_t  id;
    char     nameUtf8[128];
    int32_t  equip;         // 1..224 or 0
    int32_t  itemStore;     // 1..80 (#store) or 0
    int32_t  food;          // meal id or -1
    int32_t  quest;         // quest id or 0
    uint8_t  linkRadial;
    uint8_t  reserved[3];
} BetterINQ_SetInfo;

typedef struct BetterINQ_FoodInfo {
    uint32_t structSize;
    int32_t  id;
    char     nameUtf8[128];
    int32_t  hp;
    int32_t  stamina;
    int32_t  attack;
    int32_t  defense;
    int32_t  elemRes;
    int32_t  skills[3];
    uint8_t  deleted;
    uint8_t  reserved[3];
} BetterINQ_FoodInfo;

typedef struct BetterINQ_RestartParams {
    uint32_t structSize;
    uint32_t flags;         // BETTERINQ_RESTART_OVERRIDE_*
    int32_t  questId;
    int32_t  setId;
    uint8_t  reset;
    uint8_t  acceptOnly;
    uint8_t  wyvern;
    uint8_t  reserved;
} BetterINQ_RestartParams;

typedef struct BetterINQ_LoadoutParams {
    uint32_t structSize;
    int32_t  equip;         // 1..224, 0=skip
    int32_t  item;          // 1..80 display or #store (per flags), 0=skip
    int32_t  food;          // >0 meal id, -1 clear, 0=skip
    uint32_t flags;         // BETTERINQ_LOADOUT_*
} BetterINQ_LoadoutParams;

typedef struct BetterINQ_ConfigPatch {
    uint32_t structSize;
    uint32_t fieldMask;     // BETTERINQ_PATCH_*
    int32_t  activeSet;     // -1 or valid set id
    int32_t  questId;       // >=0
    int32_t  food;          // -1 or valid food id
    uint8_t  reset;
    uint8_t  acceptOnly;
    uint8_t  wyvern;
    uint8_t  reserved;
} BetterINQ_ConfigPatch;

typedef struct BetterINQ_CommandStatus {
    uint32_t structSize;
    uint32_t state;         // BetterINQ_CommandState
    int32_t  result;        // BetterINQ_Result
    uint32_t reserved;
} BetterINQ_CommandStatus;

typedef struct BetterINQ_ApiV1 {
    uint32_t structSize;
    uint32_t version;

    int32_t (*Release)(BetterINQ_Session session);
    int32_t (*GetStatus)(BetterINQ_Session session, BetterINQ_Status* outStatus);
    int32_t (*ListSets)(BetterINQ_Session session, BetterINQ_SetInfo* outSets, uint32_t capacity, uint32_t* outCount);
    int32_t (*ListFoods)(BetterINQ_Session session, BetterINQ_FoodInfo* outFoods, uint32_t capacity, uint32_t* outCount);
    int32_t (*ResolveItemStore)(BetterINQ_Session session, int32_t store, int32_t* outDisplay);
    int32_t (*GetCommandStatus)(BetterINQ_Session session, uint64_t requestId, BetterINQ_CommandStatus* outStatus);

    // Ephemeral session-only changes (do NOT write to BetterINQ.ini until PersistConfig)
    int32_t (*PatchSessionConfig)(BetterINQ_Session session, const BetterINQ_ConfigPatch* patch, uint64_t* outRequestId);
    int32_t (*ResetSessionOverrides)(BetterINQ_Session session, uint64_t* outRequestId);
    int32_t (*PersistConfig)(BetterINQ_Session session, uint64_t* outRequestId);

    // Actions (async queued to Better-IN-Q worker loop, respecting Active/Dormant & safety gates)
    int32_t (*RequestRestart)(BetterINQ_Session session, const BetterINQ_RestartParams* paramsOrNull, uint64_t* outRequestId);
    int32_t (*RequestResetOnly)(BetterINQ_Session session, uint64_t* outRequestId);
    int32_t (*RequestApplyActiveSet)(BetterINQ_Session session, uint64_t* outRequestId);
    int32_t (*RequestApplyLoadout)(BetterINQ_Session session, const BetterINQ_LoadoutParams* params, uint64_t* outRequestId);
    int32_t (*RequestQueueSet)(BetterINQ_Session session, int32_t setId, uint64_t* outRequestId);
    int32_t (*RequestPouchRefill)(BetterINQ_Session session, uint64_t* outRequestId);
    int32_t (*CancelPending)(BetterINQ_Session session, uint64_t* outRequestId);
} BetterINQ_ApiV1;

typedef int32_t (*BetterINQ_AcquireApiFn)(uint32_t apiVersion,
                                          const char* callerTag,
                                          BetterINQ_Session* outSession,
                                          BetterINQ_ApiV1* outApi);

#ifdef __cplusplus
}
#endif
