#ifndef USERDB_H_
#define USERDB_H_

int userdb_init(const char *db_path);
int userdb_verify_login(const char *username, const char *password);
int userdb_register_user(const char *username, const char *password, char *message, int message_len);

#endif /* USERDB_H_ */
