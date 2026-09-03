int HandleRxIntr(struct SmapDriverData *SmapDrivPrivData);
int SMAPSendPacket(const void *data, unsigned int length);
#ifdef RETROACHIEVEMENTS
int SMAPTxInit(void); /* RA: creates the transmit mutex; called at driver init */
#endif
