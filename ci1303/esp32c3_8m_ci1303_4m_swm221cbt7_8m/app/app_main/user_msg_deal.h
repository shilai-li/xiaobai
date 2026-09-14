#ifndef __USER_MSG_DEAL_H__
#define __USER_MSG_DEAL_H__

#ifdef __cplusplus
extern "C"
{
#endif



/**
 * @brief 按命令词ID响应asr消息处理
 * 
 * @param asr_msg 
 * @param cmd_handle 
 * @param cmd_id 
 * @return uint32_t 
 */
uint32_t deal_asr_msg_by_cmd_id(sys_msg_asr_data_t *asr_msg, cmd_handle_t cmd_handle, uint16_t cmd_id);

/**
 * @brief 按语义ID响应asr消息处理
 * 
 * @param asr_msg 
 * @param cmd_handle 
 * @param semantic_id 
 * @return uint32_t 
 */
uint32_t deal_asr_msg_by_semantic_id(sys_msg_asr_data_t *asr_msg, cmd_handle_t cmd_handle, uint32_t semantic_id);

/**
 * @brief 用户自定义消息处理
 * 
 * @param msg 
 * @return uint32_t 
 */
uint32_t deal_userdef_msg(sys_msg_t *msg);

/**
 * @brief 按键消息处理
 * 
 * @param msg 
 * @return uint32_t 
 */
void userapp_deal_key_msg(sys_msg_key_data_t  *key_msg);


void jw_key_init();
void jw_key_manage_task(void *p);

/* ===== SWM221 "88" Protocol ===== */
/* SWM221 System State Reporting IDs (non-conflicting with voice commands) */
#define SWM221_STATE_ONLINE    2211
#define SWM221_STATE_LISTENING 2212
#define SWM221_STATE_IDLE      2213
#define SWM221_STATE_SPEAKING  2214
/* The user is actually speaking. Reported separately from LISTENING because
 * the wake-up window that follows a topic looks identical otherwise, and the
 * SWM221 must not treat an unanswered topic as user activity. */
#define SWM221_STATE_USER_SPEAKING 2215
#define SWM221_STATE_TOPIC_SPEAKING 2216

void swm221_send_packet(uint16_t id);
void swm221_report_state(uint16_t state);
void swm221_set_sleep_timeout(uint16_t seconds);
void swm221_set_wakeup_timeout(uint16_t seconds);

#ifdef __cplusplus
}
#endif

#endif

