#include <map>
#include <list>
#include <thread>
#include "proxysql.h"
#include "cpp.h"


#ifdef DEBUG
#define DEB "_DEBUG"
#else
#define DEB ""
#endif /* DEBUG */
#define MYSQL_MONITOR_VERSION "0.2.0902" DEB


#include <event2/event.h>

extern ProxySQL_Admin *GloAdmin;
extern MySQL_Threads_Handler *GloMTH;


static MySQL_Monitor *GloMyMon;

#define NEXT_IMMEDIATE(new_st) do { ST= new_st; goto again; } while (0)

/*
#define NEXT_IMMEDIATE2(new_st) do { async_state_machine = new_st; goto handler_again; } while (0)
*/

#define SAFE_SQLITE3_STEP(_stmt) do {\
	do {\
		rc=sqlite3_step(_stmt);\
		if (rc!=SQLITE_DONE) {\
			assert(rc==SQLITE_LOCKED);\
			usleep(100);\
		}\
	} while (rc!=SQLITE_DONE);\
} while (0)

static void state_machine_handler(int fd, short event, void *arg);


static int wait_for_mysql(MYSQL *mysql, int status) {
	struct pollfd pfd;
	int timeout, res;

	pfd.fd = mysql_get_socket(mysql);
	pfd.events =
		(status & MYSQL_WAIT_READ ? POLLIN : 0) |
		(status & MYSQL_WAIT_WRITE ? POLLOUT : 0) |
		(status & MYSQL_WAIT_EXCEPT ? POLLPRI : 0);
	timeout = 10;
	res = poll(&pfd, 1, timeout);
	if (res == 0)
		return MYSQL_WAIT_TIMEOUT;
	else if (res < 0)
		return MYSQL_WAIT_TIMEOUT;
	else {
		int status = 0;
		if (pfd.revents & POLLIN) status |= MYSQL_WAIT_READ;
		if (pfd.revents & POLLOUT) status |= MYSQL_WAIT_WRITE;
		if (pfd.revents & POLLPRI) status |= MYSQL_WAIT_EXCEPT;
		return status;
	}
}

/*
static int
mysql_status2(short event, short cont) {
	int status= 0;
	if (event & POLLIN)
		status|= MYSQL_WAIT_READ;
	if (event & POLLOUT)
	status|= MYSQL_WAIT_WRITE;
//  if (event==0 && cont==true) {
//    status |= MYSQL_WAIT_TIMEOUT;
//  }
//  FIXME: handle timeout
//  if (event & PROXY_TIMEOUT)
//    status|= MYSQL_WAIT_TIMEOUT;
  return status;
}
*/
static void close_mysql(MYSQL *my) {
	if (my->net.vio) {
		char buff[5];
		mysql_hdr myhdr;
		myhdr.pkt_id=0;
		myhdr.pkt_length=1;
		memcpy(buff, &myhdr, sizeof(mysql_hdr));
		buff[4]=0x01;
		int fd=my->net.fd;
		int wb=send(fd, buff, 5, MSG_NOSIGNAL);
		fd+=wb; // dummy, to make compiler happy
		fd-=wb; // dummy, to make compiler happy
	}
	mysql_close_no_command(my);
}




/*
struct state_data {
	int ST;
	char *hostname;
	int port;
	struct event ev_mysql;
	MYSQL mysql;
	MYSQL_RES *result;
	MYSQL *ret;
	int err;
	MYSQL_ROW row;
	struct query_entry *query_element;
	int index;
};
*/

static int connect__num_active_connections;
//static int total_connect__num_active_connections=0;
static int ping__num_active_connections;
//static int total_ping__num_active_connections=0;
static int replication_lag__num_active_connections;
static int total_replication_lag__num_active_connections=0;
static int read_only__num_active_connections;
//static int total_read_only__num_active_connections=0;


struct cmp_str {
	bool operator()(char const *a, char const *b) const
	{
		return strcmp(a, b) < 0;
	}
};

class MySQL_Monitor_Connection_Pool {
	private:
	pthread_mutex_t mutex;
	int size;
	//std::map<std::pair<char *, std::list<MYSQL *>* > my_connections;
	std::map<char *, std::list<MYSQL *>* , cmp_str> my_connections;
	public:
	MySQL_Monitor_Connection_Pool();
	~MySQL_Monitor_Connection_Pool();
	MYSQL * get_connection(char *hostname, int port);
	void put_connection(char *hostname, int port, MYSQL *my);
	void purge_missing_servers(SQLite3_result *resultset);
};

MySQL_Monitor_Connection_Pool::MySQL_Monitor_Connection_Pool() {
	size=0;
	pthread_mutex_init(&mutex,NULL);
}

MySQL_Monitor_Connection_Pool::~MySQL_Monitor_Connection_Pool() {
	purge_missing_servers(NULL);
}

void MySQL_Monitor_Connection_Pool::purge_missing_servers(SQLite3_result *resultset) {
#define POLLUTE_LENGTH 8
	char pollute_buf[POLLUTE_LENGTH];
	srand(monotonic_time());
	for (int i=0; i<POLLUTE_LENGTH; i++) {
		pollute_buf[i]=(char)rand();
	}
	std::list<MYSQL *> *purge_lst=NULL;
	purge_lst=new std::list<MYSQL *>;
	pthread_mutex_lock(&mutex);
	if (resultset==NULL) {
		goto __purge_all;
	}
	for (std::vector<SQLite3_row *>::iterator it = resultset->rows.begin() ; it != resultset->rows.end(); ++it) {
		// for each host configured ...
		SQLite3_row *r=*it;
		char *buf=(char *)malloc(16+strlen(r->fields[0]));
		sprintf(buf,"%s:%s",r->fields[0],r->fields[1]);
		std::map<char *, std::list<MYSQL *>* , cmp_str >::iterator it2;
		it2 = my_connections.find(buf); // find the host
		free(buf);
		if (it2 != my_connections.end()) { // if the host exists
			std::list<MYSQL *> *lst=it2->second;
			std::list<MYSQL *>::const_iterator it3;
			for (it3 = lst->begin(); it3 != lst->end(); ++it3) {
				MYSQL *my=*it3;
				memcpy(my->net.buff,pollute_buf,8); // pollute this buffer
				//
			}
		}
	}
__purge_all:
	std::map<char *, std::list<MYSQL *>*>::iterator it;
	//std::map<std::string, std::map<std::string, std::string>>::iterator it_type;
	for(it = my_connections.begin(); it != my_connections.end(); it++) {
		std::list<MYSQL *> *lst=it->second;
		if (!lst->empty()) {
			std::list<MYSQL *>::const_iterator it3;
			it3=lst->begin();
			MYSQL *my=*it3;
			if (memcmp(my->net.buff,pollute_buf,8)) {
				// the buffer is not polluted, it means it didn't match previously
				while(!lst->empty()) {
					my=lst->front();
					lst->pop_front();
					purge_lst->push_back(my);
				}
			} else {
				// try to keep maximum 2 free connections
				// dropping all the others
				while(lst->size() > 2) {
					my=lst->front();
					lst->pop_front();
					purge_lst->push_back(my);
				}
			}
		}
	}
	pthread_mutex_unlock(&mutex);
	char quit_buff[5];
	memset(quit_buff,0,5);
	quit_buff[0]=1;
	quit_buff[4]=1;

	// close all idle connections
	while (!purge_lst->empty()) {
		MYSQL *my=purge_lst->front();
		purge_lst->pop_front();
		int fd=my->net.fd;
		int wb=write(fd,quit_buff,5);
		fd+=wb; // dummy, to make compiler happy
		fd-=wb; // dummy, to make compiler happy
		mysql_close_no_command(my);
		shutdown(fd, SHUT_RDWR);
	}
	delete purge_lst;
}

MYSQL * MySQL_Monitor_Connection_Pool::get_connection(char *hostname, int port) {
	std::map<char *, std::list<MYSQL *>* , cmp_str >::iterator it;
	//it = my_connections.find(std::make_pair(hostname,port));
	char *buf=(char *)malloc(16+strlen(hostname));
	sprintf(buf,"%s:%d",hostname,port);
	pthread_mutex_lock(&mutex);
	it = my_connections.find(buf);
	free(buf);
	if (it != my_connections.end()) {
		std::list<MYSQL *> *lst=it->second;
		if (!lst->empty()) {
			MYSQL *ret=lst->front();
			lst->pop_front();
			size--;
			pthread_mutex_unlock(&mutex);
			memset(ret->net.buff,0,8); // reset what was polluted
			return ret;
		}
	}
	pthread_mutex_unlock(&mutex);
	return NULL;
}

void MySQL_Monitor_Connection_Pool::put_connection(char *hostname, int port, MYSQL *my) {
	size++;
	std::map<char *, std::list<MYSQL *>* , cmp_str >::iterator it;
	char * buf=(char *)malloc(16+strlen(hostname));
	sprintf(buf,"%s:%d",hostname,port);
	pthread_mutex_lock(&mutex);
	it = my_connections.find(buf);
	std::list<MYSQL *> *lst=NULL;
	if (it==my_connections.end()) {
		lst=new std::list<MYSQL *>;
		my_connections.insert(my_connections.begin(), std::pair<char *,std::list<MYSQL *>*>(buf,lst));
	} else {
		free(buf);
		lst=it->second;
	}
	lst->push_back(my);
	pthread_mutex_unlock(&mutex);
}

/*
enum MySQL_Monitor_State_Data_Task_Type {
	MON_CONNECT,
	MON_PING,
	MON_READ_ONLY,
	MON_REPLICATION_LAG
};
*/

/*
class MySQL_Monitor_State_Data {
	public:
	MySQL_Monitor_State_Data_Task_Type task_id;
	struct timeval tv_out;
	unsigned long long t1;
	unsigned long long t2;
	int ST;
	char *hostname;
	int port;
	bool use_ssl;
	struct event *ev_mysql;
	MYSQL *mysql;
	struct event_base *base;
	MYSQL_RES *result;
	MYSQL *ret;
	int interr;
	char * mysql_error_msg;
	MYSQL_ROW *row;
	unsigned int repl_lag;
	unsigned int hostgroup_id;
*/
MySQL_Monitor_State_Data::MySQL_Monitor_State_Data(char *h, int p, struct event_base *b, bool _use_ssl) {
		task_id=MON_CONNECT;
		mysql=NULL;
		result=NULL;
		ret=NULL;
		row=NULL;
		mysql_error_msg=NULL;
		hostname=strdup(h);
		port=p;
		base=b;
		use_ssl=_use_ssl;
		ST=0;
		ev_mysql=NULL;
	};

MySQL_Monitor_State_Data::~MySQL_Monitor_State_Data() {
		if (hostname) {
			free(hostname);
		}
		//assert(mysql==NULL); // if mysql is not NULL, there is a bug
		if (mysql) {
			close_mysql(mysql);
			mysql=NULL;
		}
		if (mysql_error_msg) {
			free(mysql_error_msg);
		}
	}
void MySQL_Monitor_State_Data::unregister() {
		if (ev_mysql) {
			event_del(ev_mysql);
			event_free(ev_mysql);
		}
	}

int MySQL_Monitor_State_Data::handler(int fd, short event) {
		int status;
again:
		switch (ST) {
			case 0:
				mysql=mysql_init(NULL);
				assert(mysql);
				// FIXME: should we set timeout ?
				mysql_options(mysql, MYSQL_OPT_NONBLOCK, 0);
				if (use_ssl) {
					mysql_ssl_set(mysql, mysql_thread___ssl_p2s_key, mysql_thread___ssl_p2s_cert, mysql_thread___ssl_p2s_ca, NULL, mysql_thread___ssl_p2s_cipher);
				}
				if (mysql_thread___monitor_timer_cached==true) {
					event_base_gettimeofday_cached(base, &tv_out);
				} else {
					evutil_gettimeofday(&tv_out, NULL);
				}
				t1=(((unsigned long long) tv_out.tv_sec) * 1000000) + (tv_out.tv_usec);
				if (port) {
					status= mysql_real_connect_start(&ret, mysql, hostname, mysql_thread___monitor_username, mysql_thread___monitor_password, NULL, port, NULL, 0);
				} else {
					status= mysql_real_connect_start(&ret, mysql, "localhost", mysql_thread___monitor_username, mysql_thread___monitor_password, NULL, 0, hostname, 0);
				}
        if (status)
					/* Wait for connect to complete. */
					next_event(1, status);
				else
					NEXT_IMMEDIATE(3);
				break;
			case 1:
				status= mysql_real_connect_cont(&ret, mysql, mysql_status(event));
				if (status) {
					struct timeval tv_out;
					evutil_gettimeofday(&tv_out, NULL);
					unsigned long long now_time;
					now_time=(((unsigned long long) tv_out.tv_sec) * 1000000) + (tv_out.tv_usec);
					if (now_time < t1 + mysql_thread___monitor_connect_timeout * 1000) {
						next_event(1, status);
					} else {
						NEXT_IMMEDIATE(90); // we reached a timeout
					}
				}
				else
					//NEXT_IMMEDIATE(40);
					NEXT_IMMEDIATE(3);
		break;

			case 3:
				if (!ret) {
					mysql_error_msg=strdup(mysql_error(mysql));
					mysql_close(mysql);
					mysql=NULL;
					NEXT_IMMEDIATE(50);
				}
				switch(task_id) {
					case MON_CONNECT:
						NEXT_IMMEDIATE(40);
						break;
					case MON_PING:
						NEXT_IMMEDIATE(7);
						break;
					case MON_READ_ONLY:
						NEXT_IMMEDIATE(20);
						break;
					case MON_REPLICATION_LAG:
						NEXT_IMMEDIATE(10);
						break;
					default:
						assert(0);
						break;
				}
				break;

			case 7:
				if (mysql_thread___monitor_timer_cached==true) {
					event_base_gettimeofday_cached(base, &tv_out);
				} else {
					evutil_gettimeofday(&tv_out, NULL);
				}
				t1=(((unsigned long long) tv_out.tv_sec) * 1000000) + (tv_out.tv_usec);
				status=mysql_ping_start(&interr,mysql);
				if (status)
					next_event(8,status);
				else
					NEXT_IMMEDIATE(9);
				break;

			case 8:
				status=mysql_ping_cont(&interr,mysql, mysql_status(event));
				if (status) {
					struct timeval tv_out;
					evutil_gettimeofday(&tv_out, NULL);
					unsigned long long now_time;
					now_time=(((unsigned long long) tv_out.tv_sec) * 1000000) + (tv_out.tv_usec);
					if (now_time < t1 + mysql_thread___monitor_ping_timeout * 1000) {
						next_event(8,status);
					} else {
						NEXT_IMMEDIATE(90); // we reached a timeout
					}
				}
				else 
					NEXT_IMMEDIATE(9);
				break;

			case 9:
				if (interr) {
					mysql_error_msg=strdup(mysql_error(mysql));
					mysql_close(mysql);
					mysql=NULL;
					NEXT_IMMEDIATE(50);
				}
				switch(task_id) {
					case MON_PING:
					case MON_REPLICATION_LAG:
						NEXT_IMMEDIATE(39);
						break;
					default:
						assert(0);
						break;
				}
				break;

			case 90: // timeout for connect , ping or replication lag
				mysql_error_msg=strdup("timeout");
				close_mysql(mysql);
				mysql=NULL;
				return -1;
				break;

			case 10:
				if (mysql_thread___monitor_timer_cached==true) {
					event_base_gettimeofday_cached(base, &tv_out);
				} else {
					evutil_gettimeofday(&tv_out, NULL);
				}
				t1=(((unsigned long long) tv_out.tv_sec) * 1000000) + (tv_out.tv_usec);
				status=mysql_query_start(&interr,mysql,"SHOW SLAVE STATUS");
				if (status)
					next_event(11,status);
				else
					NEXT_IMMEDIATE(12);
				break;

			case 11:
				status=mysql_query_cont(&interr,mysql, mysql_status(event));
				if (status) {
					struct timeval tv_out;
					evutil_gettimeofday(&tv_out, NULL);
					unsigned long long now_time;
					now_time=(((unsigned long long) tv_out.tv_sec) * 1000000) + (tv_out.tv_usec);
					if (now_time < t1 + mysql_thread___monitor_replication_lag_timeout * 1000) {
						next_event(11,status);
					} else {
						NEXT_IMMEDIATE(90); // we reached a timeout
					}
				}
				else {
					NEXT_IMMEDIATE(12);
				}
				break;

			case 12:
				if (interr) {
					mysql_error_msg=strdup(mysql_error(mysql));
					mysql_close(mysql);
					mysql=NULL;
					NEXT_IMMEDIATE(50);
				} else {
					status=mysql_store_result_start(&result, mysql);
					if (status)
						next_event(13,status);
					else
						NEXT_IMMEDIATE(14);
				}
				break;

			case 13:
				status=mysql_store_result_cont(&result, mysql, mysql_status(event));
				if (status)
					next_event(13,status);
				else
					NEXT_IMMEDIATE(14);
				break;

			case 14:
				if (result) {
					if (mysql_thread___monitor_timer_cached==true) {
						event_base_gettimeofday_cached(base, &tv_out);
					} else {
						evutil_gettimeofday(&tv_out, NULL);
					}
					t2=(((unsigned long long) tv_out.tv_sec) * 1000000) + (tv_out.tv_usec);
					GloMyMon->My_Conn_Pool->put_connection(hostname,port,mysql);
					mysql=NULL;
					return -1;
				}	else {
					// no resultset, consider it an error
					mysql_error_msg=strdup(mysql_error(mysql));
					mysql_close(mysql);
					mysql=NULL;
					NEXT_IMMEDIATE(50);
				}
				break;

			case 20:
				if (mysql_thread___monitor_timer_cached==true) {
					event_base_gettimeofday_cached(base, &tv_out);
				} else {
					evutil_gettimeofday(&tv_out, NULL);
				}
				t1=(((unsigned long long) tv_out.tv_sec) * 1000000) + (tv_out.tv_usec);
				status=mysql_query_start(&interr,mysql,"SHOW GLOBAL VARIABLES LIKE 'read_only'");
				if (status)
					next_event(21,status);
				else
					NEXT_IMMEDIATE(22);
				break;

			case 21:
				status=mysql_query_cont(&interr,mysql, mysql_status(event));
				if (status)
					next_event(21,status);
				else
					NEXT_IMMEDIATE(22);
				break;

			case 22:
				if (interr) {
					mysql_error_msg=strdup(mysql_error(mysql));
					mysql_close(mysql);
					mysql=NULL;
					NEXT_IMMEDIATE(50);
				} else {
					status=mysql_store_result_start(&result, mysql);
					if (status)
						next_event(23,status);
					else
						NEXT_IMMEDIATE(24);
				}
				break;

			case 23:
				status=mysql_store_result_cont(&result, mysql, mysql_status(event));
				if (status)
					next_event(23,status);
				else
					NEXT_IMMEDIATE(24);
				break;

			case 24:
				if (result) {
					if (mysql_thread___monitor_timer_cached==true) {
						event_base_gettimeofday_cached(base, &tv_out);
					} else {
						evutil_gettimeofday(&tv_out, NULL);
					}
					t2=(((unsigned long long) tv_out.tv_sec) * 1000000) + (tv_out.tv_usec);
					GloMyMon->My_Conn_Pool->put_connection(hostname,port,mysql);
					mysql=NULL;
					return -1;
				}	else {
					// no resultset, consider it an error
					// FIXME: if this happen, should be logged
					mysql_error_msg=strdup(mysql_error(mysql));
					mysql_close(mysql);
					mysql=NULL;
					NEXT_IMMEDIATE(50);
				}
				break;

			case 39:
				if (mysql_thread___monitor_timer_cached==true) {
					event_base_gettimeofday_cached(base, &tv_out);
				} else {
					evutil_gettimeofday(&tv_out, NULL);
				}
				t2=(((unsigned long long) tv_out.tv_sec) * 1000000) + (tv_out.tv_usec);
				GloMyMon->My_Conn_Pool->put_connection(hostname,port,mysql);
				mysql=NULL;
				return -1;
				break;

			case 40:
				if (mysql_thread___monitor_timer_cached==true) {
					event_base_gettimeofday_cached(base, &tv_out);
				} else {
					evutil_gettimeofday(&tv_out, NULL);
				}
				t2=(((unsigned long long) tv_out.tv_sec) * 1000000) + (tv_out.tv_usec);
				NEXT_IMMEDIATE(50); // TEMP
				status= mysql_close_start(mysql);
				if (status)
					next_event(41, status);
				else
					NEXT_IMMEDIATE(50);
				break;

			case 41:
				status= mysql_close_cont(mysql, mysql_status(event));
				if (status)
					next_event(41, status);
				else
					NEXT_IMMEDIATE(50);
				break;

			case 50:
				/* We are done! */
				if (mysql) {
					mysql_close(mysql);
					mysql=NULL;
				}
				return -1;
				break;

			default:
				assert(0);
				break;

		}
		return 0;
	}
void MySQL_Monitor_State_Data::next_event(int new_st, int status) {
		short wait_event= 0;
		struct timeval tv, *ptv;
		int fd;

		if (status & MYSQL_WAIT_READ)
			wait_event|= EV_READ;
		if (status & MYSQL_WAIT_WRITE)
			wait_event|= EV_WRITE;
		if (wait_event)
			fd= mysql_get_socket(mysql);
		else
			fd= -1;
		if (status & MYSQL_WAIT_TIMEOUT) {
			tv.tv_sec= 0;
			tv.tv_usec= 10000;
			ptv= &tv;
		} else {
			ptv= NULL;
		}
		//event_set(ev_mysql, fd, wait_event, state_machine_handler, this);
		if (ev_mysql==NULL) {
			ev_mysql=event_new(base, fd, wait_event, state_machine_handler, this);
			//event_add(ev_mysql, ptv);
		}
		//event_del(ev_mysql);
		event_assign(ev_mysql, base, fd, wait_event, state_machine_handler, this);
		event_add(ev_mysql, ptv);
		ST= new_st;
	}


static void
state_machine_handler(int fd __attribute__((unused)), short event, void *arg) {
	MySQL_Monitor_State_Data *msd=(MySQL_Monitor_State_Data *)arg;
	struct event_base *base=msd->base;
	int rc=msd->handler(fd, event);
	if (rc==-1) {
		//delete msd;
		msd->unregister();
		switch (msd->task_id) {
			case MON_CONNECT:
				connect__num_active_connections--;
				if (connect__num_active_connections == 0)
					event_base_loopbreak(base);
				break;
			case MON_PING:
				ping__num_active_connections--;
				if (ping__num_active_connections == 0)
					event_base_loopbreak(base);
				break;
			case MON_READ_ONLY:
				read_only__num_active_connections--;
				if (read_only__num_active_connections == 0)
					event_base_loopbreak(base);
				break;
			case MON_REPLICATION_LAG:
				replication_lag__num_active_connections--;
				if (replication_lag__num_active_connections == 0)
					event_base_loopbreak(base);
				break;
			default:
				assert(0);
				break;
		}
	}
}

MySQL_Monitor::MySQL_Monitor() {

	GloMyMon = this;

	My_Conn_Pool=new MySQL_Monitor_Connection_Pool();

	shutdown=false;
	// create new SQLite datatabase
	monitordb = new SQLite3DB();
	monitordb->open((char *)"file:mem_monitordb?mode=memory&cache=shared", SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX);

	admindb=new SQLite3DB();
	admindb->open((char *)"file:mem_admindb?mode=memory&cache=shared", SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX);
	admindb->execute("ATTACH DATABASE 'file:mem_monitordb?mode=memory&cache=shared' AS 'monitor'");
	// define monitoring tables
	tables_defs_monitor=new std::vector<table_def_t *>;
	insert_into_tables_defs(tables_defs_monitor,"mysql_server_connect", MONITOR_SQLITE_TABLE_MYSQL_SERVER_CONNECT);	
	insert_into_tables_defs(tables_defs_monitor,"mysql_server_connect_log", MONITOR_SQLITE_TABLE_MYSQL_SERVER_CONNECT_LOG);
	insert_into_tables_defs(tables_defs_monitor,"mysql_server_ping", MONITOR_SQLITE_TABLE_MYSQL_SERVER_PING);
	insert_into_tables_defs(tables_defs_monitor,"mysql_server_ping_log", MONITOR_SQLITE_TABLE_MYSQL_SERVER_PING_LOG);
	insert_into_tables_defs(tables_defs_monitor,"mysql_server_read_only_log", MONITOR_SQLITE_TABLE_MYSQL_SERVER_READ_ONLY_LOG);
	insert_into_tables_defs(tables_defs_monitor,"mysql_server_replication_lag_log", MONITOR_SQLITE_TABLE_MYSQL_SERVER_REPLICATION_LAG_LOG);
	// create monitoring tables
	check_and_build_standard_tables(monitordb, tables_defs_monitor);
	monitordb->execute("CREATE INDEX IF NOT EXISTS idx_connect_log_time_start ON mysql_server_connect_log (time_start)");
	monitordb->execute("CREATE INDEX IF NOT EXISTS idx_ping_log_time_start ON mysql_server_ping_log (time_start)");
	monitordb->execute("CREATE INDEX IF NOT EXISTS idx_read_only_log_time_start ON mysql_server_read_only_log (time_start)");
	monitordb->execute("CREATE INDEX IF NOT EXISTS idx_replication_lag_log_time_start ON mysql_server_replication_lag_log (time_start)");


};

MySQL_Monitor::~MySQL_Monitor() {
	drop_tables_defs(tables_defs_monitor);
	delete tables_defs_monitor;
	delete monitordb;
	delete admindb;
	delete My_Conn_Pool;
};


void MySQL_Monitor::print_version() {
	fprintf(stderr,"Standard MySQL Monitor (StdMyMon) rev. %s -- %s -- %s\n", MYSQL_MONITOR_VERSION, __FILE__, __TIMESTAMP__);
};

// This function is copied from ProxySQL_Admin
void MySQL_Monitor::insert_into_tables_defs(std::vector<table_def_t *> *tables_defs, const char *table_name, const char *table_def) {
	table_def_t *td = new table_def_t;
	td->table_name=strdup(table_name);
	td->table_def=strdup(table_def);
	tables_defs->push_back(td);
};

// This function is copied from ProxySQL_Admin
void MySQL_Monitor::drop_tables_defs(std::vector<table_def_t *> *tables_defs) {
	table_def_t *td;
	while (!tables_defs->empty()) {
		td=tables_defs->back();
		free(td->table_name);
		td->table_name=NULL;
		free(td->table_def);
		td->table_def=NULL;
		tables_defs->pop_back();
		delete td;
	}
};

// This function is copied from ProxySQL_Admin
void MySQL_Monitor::check_and_build_standard_tables(SQLite3DB *db, std::vector<table_def_t *> *tables_defs) {
	table_def_t *td;
	db->execute("PRAGMA foreign_keys = OFF");
	for (std::vector<table_def_t *>::iterator it=tables_defs->begin(); it!=tables_defs->end(); ++it) {
		td=*it;
		db->check_and_build_table(td->table_name, td->table_def);
	}
	db->execute("PRAGMA foreign_keys = ON");
};

void * monitor_connect_thread(void *arg) {
	MySQL_Monitor_State_Data *mmsd=(MySQL_Monitor_State_Data *)arg;
	MySQL_Thread * mysql_thr = new MySQL_Thread();
	mysql_thr->curtime=monotonic_time();
	mysql_thr->refresh_variables();


	mmsd->create_new_connection();

	unsigned long long start_time=mysql_thr->curtime;
	mmsd->t1=start_time;
	mmsd->t2=monotonic_time();

	sqlite3_stmt *statement;
	sqlite3 *mondb=mmsd->mondb->get_db();
	int rc;
	char *query=NULL;
	query=(char *)"INSERT OR REPLACE INTO mysql_server_connect_log VALUES (?1 , ?2 , ?3 , ?4 , ?5)";
	rc=sqlite3_prepare_v2(mondb, query, -1, &statement, 0);
	assert(rc==SQLITE_OK);
	rc=sqlite3_bind_text(statement, 1, mmsd->hostname, -1, SQLITE_TRANSIENT); assert(rc==SQLITE_OK);
	rc=sqlite3_bind_int(statement, 2, mmsd->port); assert(rc==SQLITE_OK);
	rc=sqlite3_bind_int64(statement, 3, start_time); assert(rc==SQLITE_OK);
	rc=sqlite3_bind_int64(statement, 4, (mmsd->mysql_error_msg ? 0 : mmsd->t2-mmsd->t1)); assert(rc==SQLITE_OK);
	rc=sqlite3_bind_text(statement, 5, mmsd->mysql_error_msg, -1, SQLITE_TRANSIENT); assert(rc==SQLITE_OK);
	SAFE_SQLITE3_STEP(statement);
	rc=sqlite3_clear_bindings(statement); assert(rc==SQLITE_OK);
	rc=sqlite3_reset(statement); assert(rc==SQLITE_OK);
	sqlite3_finalize(statement);

	mysql_close(mmsd->mysql);
	delete mysql_thr;
	return NULL;
}

void * monitor_ping_thread(void *arg) {
	MySQL_Monitor_State_Data *mmsd=(MySQL_Monitor_State_Data *)arg;
	MySQL_Thread * mysql_thr = new MySQL_Thread();
	mysql_thr->curtime=monotonic_time();
	mysql_thr->refresh_variables();

	mmsd->mysql=GloMyMon->My_Conn_Pool->get_connection(mmsd->hostname, mmsd->port);
	unsigned long long start_time=mysql_thr->curtime;

	mmsd->t1=start_time;

	if (mmsd->mysql==NULL) { // we don't have a connection, let's create it
		bool rc;
		rc=mmsd->create_new_connection();
		if (rc==false) {
			goto __exit_monitor_ping_thread;
		}
	}

	mmsd->t1=monotonic_time();
	//async_exit_status=mysql_change_user_start(&ret_bool, mysql,"msandbox2","msandbox2","information_schema");
	mmsd->async_exit_status=mysql_ping_start(&mmsd->interr,mmsd->mysql);
	while (mmsd->async_exit_status) {
		mmsd->async_exit_status=wait_for_mysql(mmsd->mysql, mmsd->async_exit_status);
		mmsd->async_exit_status=mysql_ping_cont(&mmsd->interr, mmsd->mysql, mmsd->async_exit_status);
		unsigned long long now=monotonic_time();
		if (now > mmsd->t1 + mysql_thread___monitor_ping_timeout * 1000) {
			mmsd->mysql_error_msg=strdup("timeout during ping");
			goto __exit_monitor_ping_thread;
		}
		if (GloMyMon->shutdown==true) {
			goto __fast_exit_monitor_ping_thread;	// exit immediately
		}
	}
	if (mmsd->interr) { // ping failed
		mmsd->mysql_error_msg=strdup(mysql_error(mmsd->mysql));
	} else {
		GloMyMon->My_Conn_Pool->put_connection(mmsd->hostname,mmsd->port,mmsd->mysql);
		mmsd->mysql=NULL;
	}

__exit_monitor_ping_thread:
	mmsd->t2=monotonic_time();
	{
		sqlite3_stmt *statement;
		sqlite3 *mondb=mmsd->mondb->get_db();
		int rc;
		char *query=NULL;
		query=(char *)"INSERT OR REPLACE INTO mysql_server_ping_log VALUES (?1 , ?2 , ?3 , ?4 , ?5)";
		rc=sqlite3_prepare_v2(mondb, query, -1, &statement, 0);
		assert(rc==SQLITE_OK);
		rc=sqlite3_bind_text(statement, 1, mmsd->hostname, -1, SQLITE_TRANSIENT); assert(rc==SQLITE_OK);
		rc=sqlite3_bind_int(statement, 2, mmsd->port); assert(rc==SQLITE_OK);
		rc=sqlite3_bind_int64(statement, 3, start_time); assert(rc==SQLITE_OK);
		rc=sqlite3_bind_int64(statement, 4, (mmsd->mysql_error_msg ? 0 : mmsd->t2-mmsd->t1)); assert(rc==SQLITE_OK);
		rc=sqlite3_bind_text(statement, 5, mmsd->mysql_error_msg, -1, SQLITE_TRANSIENT); assert(rc==SQLITE_OK);
		SAFE_SQLITE3_STEP(statement);
		rc=sqlite3_clear_bindings(statement); assert(rc==SQLITE_OK);
		rc=sqlite3_reset(statement); assert(rc==SQLITE_OK);
		sqlite3_finalize(statement);
	}
__fast_exit_monitor_ping_thread:
	if (mmsd->mysql) {
		mysql_close(mmsd->mysql); // if we reached here we didn't put the connection back
	}
	delete mysql_thr;
	return NULL;
}

bool MySQL_Monitor_State_Data::create_new_connection() {
		mysql=mysql_init(NULL);
		assert(mysql);
		if (use_ssl) {
			mysql_ssl_set(mysql, mysql_thread___ssl_p2s_key, mysql_thread___ssl_p2s_cert, mysql_thread___ssl_p2s_ca, NULL, mysql_thread___ssl_p2s_cipher);
		}
		unsigned int timeout=mysql_thread___monitor_connect_timeout/1000;
		mysql_options(mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
//		mysql_options(mysql, MYSQL_OPT_READ_TIMEOUT, &timeout);
//		mysql_options(mysql, MYSQL_OPT_WRITE_TIMEOUT, &timeout);
		MYSQL *myrc=NULL;
		if (port) {
			myrc=mysql_real_connect(mysql, hostname, mysql_thread___monitor_username, mysql_thread___monitor_password, NULL, port, NULL, 0);
		} else {
			myrc=mysql_real_connect(mysql, "localhost", mysql_thread___monitor_username, mysql_thread___monitor_password, NULL, 0, hostname, 0);
		}
		if (myrc==NULL) {
			mysql_error_msg=strdup(mysql_error(mysql));
			return false;
		} else {
			// mariadb client library disables NONBLOCK for SSL connections ... re-enable it!
			mysql_options(mysql, MYSQL_OPT_NONBLOCK, 0);
			int f=fcntl(mysql->net.fd, F_GETFL);
#ifdef FD_CLOEXEC
			// asynchronously set also FD_CLOEXEC , this to prevent then when a fork happens the FD are duplicated to new process
			fcntl(mysql->net.fd, F_SETFL, f|O_NONBLOCK|FD_CLOEXEC);
#else
			fcntl(mysql->net.fd, F_SETFL, f|O_NONBLOCK);
#endif /* FD_CLOEXEC */
	}
	return true;
}

void * monitor_read_only_thread(void *arg) {
	MySQL_Monitor_State_Data *mmsd=(MySQL_Monitor_State_Data *)arg;
	MySQL_Thread * mysql_thr = new MySQL_Thread();
	mysql_thr->curtime=monotonic_time();
	mysql_thr->refresh_variables();

	mmsd->mysql=GloMyMon->My_Conn_Pool->get_connection(mmsd->hostname, mmsd->port);
	unsigned long long start_time=mysql_thr->curtime;


	mmsd->t1=start_time;

	if (mmsd->mysql==NULL) { // we don't have a connection, let's create it
		bool rc;
		rc=mmsd->create_new_connection();
		if (rc==false) {
			goto __exit_monitor_read_only_thread;
		}
	}

	mmsd->t1=monotonic_time();
	//async_exit_status=mysql_change_user_start(&ret_bool, mysql,"msandbox2","msandbox2","information_schema");
	//mmsd->async_exit_status=mysql_ping_start(&mmsd->interr,mmsd->mysql);
	mmsd->async_exit_status=mysql_query_start(&mmsd->interr,mmsd->mysql,"SHOW GLOBAL VARIABLES LIKE 'read_only'");
	while (mmsd->async_exit_status) {
		mmsd->async_exit_status=wait_for_mysql(mmsd->mysql, mmsd->async_exit_status);
		mmsd->async_exit_status=mysql_query_cont(&mmsd->interr, mmsd->mysql, mmsd->async_exit_status);
		unsigned long long now=monotonic_time();
		if (now > mmsd->t1 + mysql_thread___monitor_ping_timeout * 1000) {
			mmsd->mysql_error_msg=strdup("timeout check");
			goto __exit_monitor_read_only_thread;
		}
		if (GloMyMon->shutdown==true) {
			goto __fast_exit_monitor_read_only_thread;	// exit immediately
		}
	}
	mmsd->async_exit_status=mysql_store_result_start(&mmsd->result,mmsd->mysql);
	while (mmsd->async_exit_status) {
		mmsd->async_exit_status=wait_for_mysql(mmsd->mysql, mmsd->async_exit_status);
		mmsd->async_exit_status=mysql_store_result_cont(&mmsd->result, mmsd->mysql, mmsd->async_exit_status);
		unsigned long long now=monotonic_time();
		if (now > mmsd->t1 + mysql_thread___monitor_ping_timeout * 1000) {
			mmsd->mysql_error_msg=strdup("timeout check");
			goto __exit_monitor_read_only_thread;
		}
		if (GloMyMon->shutdown==true) {
			goto __fast_exit_monitor_read_only_thread;	// exit immediately
		}
	}
	if (mmsd->interr) { // ping failed
		mmsd->mysql_error_msg=strdup(mysql_error(mmsd->mysql));
//	} else {
//		GloMyMon->My_Conn_Pool->put_connection(mmsd->hostname,mmsd->port,mmsd->mysql);
//		mmsd->mysql=NULL;
	}

__exit_monitor_read_only_thread:
	mmsd->t2=monotonic_time();
	{
		sqlite3_stmt *statement;
		sqlite3 *mondb=mmsd->mondb->get_db();
		int rc;
		char *query=NULL;
		query=(char *)"INSERT OR REPLACE INTO mysql_server_read_only_log VALUES (?1 , ?2 , ?3 , ?4 , ?5 , ?6)";
		rc=sqlite3_prepare_v2(mondb, query, -1, &statement, 0);
		assert(rc==SQLITE_OK);
		int read_only=1; // as a safety mechanism , read_only=1 is the default
		rc=sqlite3_bind_text(statement, 1, mmsd->hostname, -1, SQLITE_TRANSIENT); assert(rc==SQLITE_OK);
		rc=sqlite3_bind_int(statement, 2, mmsd->port); assert(rc==SQLITE_OK);
		rc=sqlite3_bind_int64(statement, 3, start_time); assert(rc==SQLITE_OK);
		rc=sqlite3_bind_int64(statement, 4, (mmsd->mysql_error_msg ? 0 : mmsd->t2-mmsd->t1)); assert(rc==SQLITE_OK);
		if (mmsd->result) {
			int num_fields=0;
			int k=0;
			MYSQL_FIELD *fields=NULL;
			int j=-1;
			num_fields = mysql_num_fields(mmsd->result);
			fields = mysql_fetch_fields(mmsd->result);
			for(k = 0; k < num_fields; k++) {
				//if (strcmp("VARIABLE_NAME", fields[k].name)==0) {
				if (strcmp("Value", fields[k].name)==0) {
					j=k;
				}
			}
			if (j>-1) {
				MYSQL_ROW row=mysql_fetch_row(mmsd->result);
				if (row) {
					if (row[j]) {
						if (!strcmp(row[j],"0") || !strcasecmp(row[j],"OFF"))
							read_only=0;
					}
				}
			}
//					if (repl_lag>=0) {
			rc=sqlite3_bind_int64(statement, 5, read_only); assert(rc==SQLITE_OK);
//					} else {
//						rc=sqlite3_bind_null(statement, 5); assert(rc==SQLITE_OK);
//					}
			mysql_free_result(mmsd->result);
			mmsd->result=NULL;
		} else {
			rc=sqlite3_bind_null(statement, 5); assert(rc==SQLITE_OK);
		}
		rc=sqlite3_bind_text(statement, 6, mmsd->mysql_error_msg, -1, SQLITE_TRANSIENT); assert(rc==SQLITE_OK);
		SAFE_SQLITE3_STEP(statement);
		rc=sqlite3_clear_bindings(statement); assert(rc==SQLITE_OK);
		rc=sqlite3_reset(statement); assert(rc==SQLITE_OK);

		MyHGM->read_only_action(mmsd->hostname, mmsd->port, read_only);

		sqlite3_finalize(statement);
	}
	if (mmsd->interr) { // check failed
	} else {
		GloMyMon->My_Conn_Pool->put_connection(mmsd->hostname,mmsd->port,mmsd->mysql);
		mmsd->mysql=NULL;
	}
__fast_exit_monitor_read_only_thread:
	if (mmsd->mysql) {
		mysql_close(mmsd->mysql); // if we reached here we didn't put the connection back
	}
	delete mysql_thr;
	return NULL;
}


void * MySQL_Monitor::monitor_connect() {
	// initialize the MySQL Thread (note: this is not a real thread, just the structures associated with it)
	//struct event_base *libevent_base;
	unsigned int MySQL_Monitor__thread_MySQL_Thread_Variables_version;
	MySQL_Thread * mysql_thr = new MySQL_Thread();
	mysql_thr->curtime=monotonic_time();
	MySQL_Monitor__thread_MySQL_Thread_Variables_version=GloMTH->get_global_version();
	mysql_thr->refresh_variables();


	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
//	pthread_attr_setstacksize (&attr, 192*1024);


	unsigned long long t1;
	unsigned long long t2;
	unsigned long long next_loop_at=0;
	unsigned long long start_time;
	while (shutdown==false) {

		char *error=NULL;
		int cols=0;
		int affected_rows=0;
		SQLite3_result *resultset=NULL;
		// add support for SSL
		char *query=(char *)"SELECT hostname, port, MAX(use_ssl) use_ssl FROM mysql_servers GROUP BY hostname, port";
		unsigned int glover;
		t1=monotonic_time();

		glover=GloMTH->get_global_version();
		if (MySQL_Monitor__thread_MySQL_Thread_Variables_version < glover ) {
			MySQL_Monitor__thread_MySQL_Thread_Variables_version=glover;
			mysql_thr->refresh_variables();
			next_loop_at=0;
		}

		if (t1 < next_loop_at) {
			goto __sleep_monitor_connect_loop;
		}
		next_loop_at=t1+1000*mysql_thread___monitor_connect_interval;

		start_time=monotonic_time();

		proxy_debug(PROXY_DEBUG_ADMIN, 4, "%s\n", query);
		admindb->execute_statement(query, &error , &cols , &affected_rows , &resultset);
		if (error) {
			proxy_error("Error on %s : %s\n", query, error);
			goto __end_monitor_connect_loop;
		} else {
			GloMyMon->My_Conn_Pool->purge_missing_servers(resultset);
			if (resultset->rows_count==0) {
				goto __end_monitor_connect_loop;
			}
			for (std::vector<SQLite3_row *>::iterator it = resultset->rows.begin() ; it != resultset->rows.end(); ++it) {
				SQLite3_row *r=*it;
				MySQL_Monitor_State_Data *mmsd=new MySQL_Monitor_State_Data(r->fields[0],atoi(r->fields[1]), NULL, atoi(r->fields[2]));
				mmsd->mondb=monitordb;
				pthread_t thr_;
				if ( pthread_create(&thr_, &attr, monitor_connect_thread, (void *)mmsd) != 0 ) {
					perror("Thread creation monitor_connect_thread");
				}
			}
		}


__end_monitor_connect_loop:
		/* if (sds) */ {
			sqlite3_stmt *statement;
			sqlite3 *mondb=monitordb->get_db();
			int rc;
			char *query=NULL;
			query=(char *)"DELETE FROM mysql_server_connect_log WHERE time_start < ?1";
			rc=sqlite3_prepare_v2(mondb, query, -1, &statement, 0);
			assert(rc==SQLITE_OK);
			if (mysql_thread___monitor_history < mysql_thread___monitor_ping_interval * (mysql_thread___monitor_ping_max_failures + 1 )) { // issue #626
				if (mysql_thread___monitor_ping_interval < 3600000)
					mysql_thread___monitor_history = mysql_thread___monitor_ping_interval * (mysql_thread___monitor_ping_max_failures + 1 );
			}
			rc=sqlite3_bind_int64(statement, 1, start_time-mysql_thread___monitor_history*1000); assert(rc==SQLITE_OK);
			SAFE_SQLITE3_STEP(statement);
			rc=sqlite3_clear_bindings(statement); assert(rc==SQLITE_OK);
			rc=sqlite3_reset(statement); assert(rc==SQLITE_OK);
			sqlite3_finalize(statement);
		}
		if (resultset)
			delete resultset;

__sleep_monitor_connect_loop:
		t2=monotonic_time();
		if (t2<next_loop_at) {
			unsigned long long st=0;
			st=next_loop_at-t2;
			if (st > 500000) {
				st = 500000;
			}
			usleep(st);
		}
	}
	if (mysql_thr) {
		delete mysql_thr;
		mysql_thr=NULL;
	}
	return NULL;
}


void * MySQL_Monitor::monitor_ping() {
	// initialize the MySQL Thread (note: this is not a real thread, just the structures associated with it)
//	struct event_base *libevent_base;
	unsigned int MySQL_Monitor__thread_MySQL_Thread_Variables_version;
	MySQL_Thread * mysql_thr = new MySQL_Thread();
	mysql_thr->curtime=monotonic_time();
	MySQL_Monitor__thread_MySQL_Thread_Variables_version=GloMTH->get_global_version();
	mysql_thr->refresh_variables();

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
//	pthread_attr_setstacksize (&attr, 192*1024);

	unsigned long long t1;
	unsigned long long t2;
	unsigned long long start_time;
	unsigned long long next_loop_at=0;

	while (shutdown==false) {

		unsigned int glover;
		char *error=NULL;
		int cols=0;
		int affected_rows=0;
		SQLite3_result *resultset=NULL;
		char *query=(char *)"SELECT hostname, port, MAX(use_ssl) use_ssl FROM mysql_servers WHERE status!='OFFLINE_HARD' GROUP BY hostname, port";
		t1=monotonic_time();

		glover=GloMTH->get_global_version();
		if (MySQL_Monitor__thread_MySQL_Thread_Variables_version < glover ) {
			MySQL_Monitor__thread_MySQL_Thread_Variables_version=glover;
			mysql_thr->refresh_variables();
			next_loop_at=0;
		}

		if (t1 < next_loop_at) {
			goto __sleep_monitor_ping_loop;
		}
		next_loop_at=t1+1000*mysql_thread___monitor_ping_interval;

		start_time=monotonic_time();

		proxy_debug(PROXY_DEBUG_ADMIN, 4, "%s\n", query);
		admindb->execute_statement(query, &error , &cols , &affected_rows , &resultset);
		if (error) {
			proxy_error("Error on %s : %s\n", query, error);
			goto __end_monitor_ping_loop;
		} else {
			if (resultset->rows_count==0) {
				goto __end_monitor_ping_loop;
			}
			for (std::vector<SQLite3_row *>::iterator it = resultset->rows.begin() ; it != resultset->rows.end(); ++it) {
				SQLite3_row *r=*it;
				MySQL_Monitor_State_Data *mmsd = new MySQL_Monitor_State_Data(r->fields[0],atoi(r->fields[1]), NULL, atoi(r->fields[2]));
				mmsd->mondb=monitordb;
				pthread_t thr_;
				if ( pthread_create(&thr_, &attr, monitor_ping_thread, (void *)mmsd) != 0 ) {
					perror("Thread creation monitor_ping_thread");
				}
			}
		}

__end_monitor_ping_loop:
		/* if (sds) */ {
			sqlite3_stmt *statement;
			sqlite3 *mondb=monitordb->get_db();
			int rc;
			char *query=NULL;
			query=(char *)"DELETE FROM mysql_server_ping_log WHERE time_start < ?1";
			rc=sqlite3_prepare_v2(mondb, query, -1, &statement, 0);
			assert(rc==SQLITE_OK);
			if (mysql_thread___monitor_history < mysql_thread___monitor_ping_interval * (mysql_thread___monitor_ping_max_failures + 1 )) { // issue #626
				if (mysql_thread___monitor_ping_interval < 3600000)
					mysql_thread___monitor_history = mysql_thread___monitor_ping_interval * (mysql_thread___monitor_ping_max_failures + 1 );
			}
			rc=sqlite3_bind_int64(statement, 1, start_time-mysql_thread___monitor_history*1000); assert(rc==SQLITE_OK);
			SAFE_SQLITE3_STEP(statement);
			rc=sqlite3_clear_bindings(statement); assert(rc==SQLITE_OK);
			rc=sqlite3_reset(statement); assert(rc==SQLITE_OK);
			sqlite3_finalize(statement);
		}

		if (resultset) {
			delete resultset;
			resultset=NULL;
		}

		// now it is time to shun all problematic hosts
		query=(char *)"SELECT DISTINCT a.hostname, a.port FROM mysql_servers a JOIN monitor.mysql_server_ping_log b ON a.hostname=b.hostname WHERE status!='OFFLINE_HARD' AND b.ping_error IS NOT NULL";
		proxy_debug(PROXY_DEBUG_ADMIN, 4, "%s\n", query);
		admindb->execute_statement(query, &error , &cols , &affected_rows , &resultset);
		if (error) {
			proxy_error("Error on %s : %s\n", query, error);
		} else {
			// get all addresses and ports
			int i=0;
			int j=0;
			char **addresses=(char **)malloc(resultset->rows_count * sizeof(char *));
			char **ports=(char **)malloc(resultset->rows_count * sizeof(char *));
			for (std::vector<SQLite3_row *>::iterator it = resultset->rows.begin() ; it != resultset->rows.end(); ++it) {
				SQLite3_row *r=*it;
				addresses[i]=strdup(r->fields[0]);
				ports[i]=strdup(r->fields[1]);
				i++;
			}
			if (resultset) {
				delete resultset;
				resultset=NULL;
			}
			char *new_query=NULL;
			new_query=(char *)"SELECT 1 FROM (SELECT hostname,port,ping_error FROM mysql_server_ping_log WHERE hostname='%s' AND port='%s' ORDER BY time_start DESC LIMIT %d) a WHERE ping_error IS NOT NULL GROUP BY hostname,port HAVING COUNT(*)=%d";
			for (j=0;j<i;j++) {
				char *buff=(char *)malloc(strlen(new_query)+strlen(addresses[j])+strlen(ports[j])+16);
				int max_failures=mysql_thread___monitor_ping_max_failures;
				sprintf(buff,new_query,addresses[j],ports[j],max_failures,max_failures);
				monitordb->execute_statement(buff, &error , &cols , &affected_rows , &resultset);
				if (!error) {
					if (resultset) {
						if (resultset->rows_count) {
							// disable host
							proxy_error("Server %s:%s missed %d heartbeats, shunning it and killing all the connections\n", addresses[j], ports[j], max_failures);
							MyHGM->shun_and_killall(addresses[j],atoi(ports[j]));
						}
						delete resultset;
						resultset=NULL;
					}
				} else {
					proxy_error("Error on %s : %s\n", query, error);
				}
				free(buff);
			}

			while (i) { // now free all the addresses/ports
				i--;
				free(addresses[i]);
				free(ports[i]);
			}
			free(addresses);
			free(ports);
		}


		// now it is time to update current_lantency_ms
		query=(char *)"SELECT DISTINCT a.hostname, a.port FROM mysql_servers a JOIN monitor.mysql_server_ping_log b ON a.hostname=b.hostname WHERE status!='OFFLINE_HARD' AND b.ping_error IS NULL";
		proxy_debug(PROXY_DEBUG_ADMIN, 4, "%s\n", query);
		admindb->execute_statement(query, &error , &cols , &affected_rows , &resultset);
		if (error) {
			proxy_error("Error on %s : %s\n", query, error);
		} else {
			// get all addresses and ports
			int i=0;
			int j=0;
			char **addresses=(char **)malloc(resultset->rows_count * sizeof(char *));
			char **ports=(char **)malloc(resultset->rows_count * sizeof(char *));
			for (std::vector<SQLite3_row *>::iterator it = resultset->rows.begin() ; it != resultset->rows.end(); ++it) {
				SQLite3_row *r=*it;
				addresses[i]=strdup(r->fields[0]);
				ports[i]=strdup(r->fields[1]);
				i++;
			}
			if (resultset) {
				delete resultset;
				resultset=NULL;
			}
			char *new_query=NULL;

			new_query=(char *)"SELECT hostname,port,COALESCE(CAST(AVG(ping_success_time) AS INTEGER),10000) FROM (SELECT hostname,port,ping_success_time,ping_error FROM mysql_server_ping_log WHERE hostname='%s' AND port='%s' ORDER BY time_start DESC LIMIT 3) a WHERE ping_error IS NULL GROUP BY hostname,port";
			for (j=0;j<i;j++) {
				char *buff=(char *)malloc(strlen(new_query)+strlen(addresses[j])+strlen(ports[j])+16);
				sprintf(buff,new_query,addresses[j],ports[j]);
				monitordb->execute_statement(buff, &error , &cols , &affected_rows , &resultset);
				if (!error) {
					if (resultset) {
						if (resultset->rows_count) {
							for (std::vector<SQLite3_row *>::iterator it = resultset->rows.begin() ; it != resultset->rows.end(); ++it) {
								SQLite3_row *r=*it; // this should be called just once, but we create a generic for loop
								// update current_latency_ms
								MyHGM->set_server_current_latency_us(addresses[j],atoi(ports[j]), atoi(r->fields[2]));
							}
						}
						delete resultset;
						resultset=NULL;
					}
				} else {
					proxy_error("Error on %s : %s\n", query, error);
				}
				free(buff);
			}
		}

__sleep_monitor_ping_loop:
		t2=monotonic_time();
		if (t2<next_loop_at) {
			unsigned long long st=0;
			st=next_loop_at-t2;
			if (st > 500000) {
				st = 500000;
			}
			usleep(st);
		}
	}
	if (mysql_thr) {
		delete mysql_thr;
		mysql_thr=NULL;
	}
	return NULL;
}

void * MySQL_Monitor::monitor_read_only() {
	// initialize the MySQL Thread (note: this is not a real thread, just the structures associated with it)
//	struct event_base *libevent_base;
	unsigned int MySQL_Monitor__thread_MySQL_Thread_Variables_version;
	MySQL_Thread * mysql_thr = new MySQL_Thread();
	mysql_thr->curtime=monotonic_time();
	MySQL_Monitor__thread_MySQL_Thread_Variables_version=GloMTH->get_global_version();
	mysql_thr->refresh_variables();

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
//	pthread_attr_setstacksize (&attr, 192*1024);

	unsigned long long t1;
	unsigned long long t2;
	unsigned long long start_time;
	unsigned long long next_loop_at=0;

	while (shutdown==false) {

		unsigned int glover;
		char *error=NULL;
		SQLite3_result *resultset=NULL;
		//char *query=(char *)"SELECT DISTINCT hostname, port FROM mysql_servers JOIN mysql_replication_hostgroups ON hostgroup_id=writer_hostgroup OR hostgroup_id=reader_hostgroup WHERE status!='OFFLINE_HARD'";
		// add support for SSL
		char *query=(char *)"SELECT hostname, port, MAX(use_ssl) use_ssl FROM mysql_servers JOIN mysql_replication_hostgroups ON hostgroup_id=writer_hostgroup OR hostgroup_id=reader_hostgroup WHERE status!='OFFLINE_HARD' GROUP BY hostname, port";
		t1=monotonic_time();
		start_time=t1;

		glover=GloMTH->get_global_version();
		if (MySQL_Monitor__thread_MySQL_Thread_Variables_version < glover ) {
			MySQL_Monitor__thread_MySQL_Thread_Variables_version=glover;
			mysql_thr->refresh_variables();
			next_loop_at=0;
		}

		if (t1 < next_loop_at) {
			goto __sleep_monitor_read_only;
		}
		next_loop_at=t1+1000*mysql_thread___monitor_read_only_interval;
		proxy_debug(PROXY_DEBUG_ADMIN, 4, "%s\n", query);
//		admindb->execute_statement(query, &error , &cols , &affected_rows , &resultset);
		resultset = MyHGM->execute_query(query, &error);
		assert(resultset);
		if (error) {
			proxy_error("Error on %s : %s\n", query, error);
			goto __end_monitor_read_only_loop;
		} else {
			if (resultset->rows_count==0) {
				goto __end_monitor_read_only_loop;
			}
			for (std::vector<SQLite3_row *>::iterator it = resultset->rows.begin() ; it != resultset->rows.end(); ++it) {
				SQLite3_row *r=*it;
				MySQL_Monitor_State_Data *mmsd=new MySQL_Monitor_State_Data(r->fields[0],atoi(r->fields[1]), NULL, atoi(r->fields[2]));
				mmsd->mondb=monitordb;
				pthread_t thr_;
				if ( pthread_create(&thr_, &attr, monitor_read_only_thread, (void *)mmsd) != 0 ) {
					perror("Thread creation monitor_read_only_thread");
				}
			}
		}

__end_monitor_read_only_loop:
			/* if (sds) */ {
			sqlite3_stmt *statement=NULL;
			sqlite3 *mondb=monitordb->get_db();
			int rc;
			char *query=NULL;
			query=(char *)"DELETE FROM mysql_server_read_only_log WHERE time_start < ?1";
			rc=sqlite3_prepare_v2(mondb, query, -1, &statement, 0);
			assert(rc==SQLITE_OK);
			if (mysql_thread___monitor_history < mysql_thread___monitor_ping_interval * (mysql_thread___monitor_ping_max_failures + 1 )) { // issue #626
				if (mysql_thread___monitor_ping_interval < 3600000)
					mysql_thread___monitor_history = mysql_thread___monitor_ping_interval * (mysql_thread___monitor_ping_max_failures + 1 );
			}
			rc=sqlite3_bind_int64(statement, 1, start_time-mysql_thread___monitor_history*1000); assert(rc==SQLITE_OK);
			SAFE_SQLITE3_STEP(statement);
			rc=sqlite3_clear_bindings(statement); assert(rc==SQLITE_OK);
			rc=sqlite3_reset(statement); assert(rc==SQLITE_OK);
			sqlite3_finalize(statement);
		}


		if (resultset)
			delete resultset;



__sleep_monitor_read_only:
		t2=monotonic_time();
		if (t2<next_loop_at) {
			unsigned long long st=0;
			st=next_loop_at-t2;
			if (st > 500000) {
				st = 500000;
			}
			usleep(st);
		}
	}
	if (mysql_thr) {
		delete mysql_thr;
		mysql_thr=NULL;
	}
	return NULL;
}

void * MySQL_Monitor::monitor_replication_lag() {
	// initialize the MySQL Thread (note: this is not a real thread, just the structures associated with it)
	struct event_base *libevent_base;
	unsigned int MySQL_Monitor__thread_MySQL_Thread_Variables_version;
	MySQL_Thread * mysql_thr = new MySQL_Thread();
	mysql_thr->curtime=monotonic_time();
	MySQL_Monitor__thread_MySQL_Thread_Variables_version=GloMTH->get_global_version();
	mysql_thr->refresh_variables();

	unsigned long long t1;
	unsigned long long t2;
	unsigned long long start_time;
	unsigned long long next_loop_at=0;

	unsigned int num_fields=0;
	unsigned int k=0;
	MYSQL_FIELD *fields=NULL;

	while (shutdown==false) {

		unsigned int glover;
		char *error=NULL;
//		int cols=0;
//		int affected_rows=0;
		SQLite3_result *resultset=NULL;
		MySQL_Monitor_State_Data **sds=NULL;
		int i=0;
		//char *query=(char *)"SELECT hostgroup_id, hostname, port, max_replication_lag FROM mysql_servers WHERE max_replication_lag > 0 AND status NOT LIKE 'OFFLINE%'";
		// add support for SSL
		char *query=(char *)"SELECT hostgroup_id, hostname, port, max_replication_lag, use_ssl FROM mysql_servers WHERE max_replication_lag > 0 AND status NOT LIKE 'OFFLINE%'";
		t1=monotonic_time();

		glover=GloMTH->get_global_version();
		if (MySQL_Monitor__thread_MySQL_Thread_Variables_version < glover ) {
			MySQL_Monitor__thread_MySQL_Thread_Variables_version=glover;
			mysql_thr->refresh_variables();
			next_loop_at=0;
		}

		if (t1 < next_loop_at) {
			goto __sleep_monitor_replication_lag;
		}
		next_loop_at=t1+1000*mysql_thread___monitor_replication_lag_interval;

		struct timeval tv_out;
		evutil_gettimeofday(&tv_out, NULL);
		start_time=(((unsigned long long) tv_out.tv_sec) * 1000000) + (tv_out.tv_usec);

		replication_lag__num_active_connections=0;
		// create libevent base
		libevent_base= event_base_new();

		proxy_debug(PROXY_DEBUG_ADMIN, 4, "%s\n", query);
//		admindb->execute_statement(query, &error , &cols , &affected_rows , &resultset);
		resultset = MyHGM->execute_query(query, &error);
		assert(resultset);
		if (error) {
			proxy_error("Error on %s : %s\n", query, error);
			goto __end_monitor_replication_lag_loop;
		} else {
			if (resultset->rows_count==0) {
				goto __end_monitor_replication_lag_loop;
			}
			sds=(MySQL_Monitor_State_Data **)malloc(resultset->rows_count * sizeof(MySQL_Monitor_State_Data *));
			for (std::vector<SQLite3_row *>::iterator it = resultset->rows.begin() ; it != resultset->rows.end(); ++it) {
				SQLite3_row *r=*it;
				sds[i] = new MySQL_Monitor_State_Data(r->fields[1],atoi(r->fields[2]),libevent_base, atoi(r->fields[4]));
				sds[i]->task_id=MON_REPLICATION_LAG;
				sds[i]->hostgroup_id=atoi(r->fields[0]);
				sds[i]->repl_lag=atoi(r->fields[3]);
				replication_lag__num_active_connections++;
				total_replication_lag__num_active_connections++;
				MySQL_Monitor_State_Data *_mmsd=sds[i];
				_mmsd->mysql=GloMyMon->My_Conn_Pool->get_connection(_mmsd->hostname, _mmsd->port);
				if (_mmsd->mysql==NULL) {
					state_machine_handler(-1,-1,_mmsd);
				} else {
					int fd=mysql_get_socket(_mmsd->mysql);
					_mmsd->ST=10;
					state_machine_handler(fd,-1,_mmsd);
				}
				i++;
			}
		}

		// start libevent loop
		event_base_dispatch(libevent_base);

__end_monitor_replication_lag_loop:
		if (sds) {
			sqlite3_stmt *statement;
			sqlite3 *mondb=monitordb->get_db();
			int rc;
			char *query=NULL;
			query=(char *)"DELETE FROM mysql_server_replication_lag_log WHERE time_start < ?1";
			rc=sqlite3_prepare_v2(mondb, query, -1, &statement, 0);
			assert(rc==SQLITE_OK);
			if (mysql_thread___monitor_history < mysql_thread___monitor_ping_interval * (mysql_thread___monitor_ping_max_failures + 1 )) { // issue #626
				if (mysql_thread___monitor_ping_interval < 3600000)
					mysql_thread___monitor_history = mysql_thread___monitor_ping_interval * (mysql_thread___monitor_ping_max_failures + 1 );
			}
			rc=sqlite3_bind_int64(statement, 1, start_time-mysql_thread___monitor_history*1000); assert(rc==SQLITE_OK);
			SAFE_SQLITE3_STEP(statement);
			rc=sqlite3_clear_bindings(statement); assert(rc==SQLITE_OK);
			rc=sqlite3_reset(statement); assert(rc==SQLITE_OK);
			sqlite3_finalize(statement);

			query=(char *)"INSERT OR REPLACE INTO mysql_server_replication_lag_log VALUES (?1 , ?2 , ?3 , ?4 , ?5 , ?6)";
			rc=sqlite3_prepare_v2(mondb, query, -1, &statement, 0);
			assert(rc==SQLITE_OK);
			while (i>0) {
				i--;
				int repl_lag=-2;
				MySQL_Monitor_State_Data *mmsd=sds[i];
				rc=sqlite3_bind_text(statement, 1, mmsd->hostname, -1, SQLITE_TRANSIENT); assert(rc==SQLITE_OK);
				rc=sqlite3_bind_int(statement, 2, mmsd->port); assert(rc==SQLITE_OK);
				rc=sqlite3_bind_int64(statement, 3, start_time); assert(rc==SQLITE_OK);
				rc=sqlite3_bind_int64(statement, 4, (mmsd->mysql_error_msg ? 0 : mmsd->t2-mmsd->t1)); assert(rc==SQLITE_OK);
				if (mmsd->result) {
					num_fields=0;
					k=0;
					fields=NULL;
					int j=-1;
					num_fields = mysql_num_fields(mmsd->result);
					fields = mysql_fetch_fields(mmsd->result);
					for(k = 0; k < num_fields; k++) {
						if (strcmp("Seconds_Behind_Master", fields[k].name)==0) {
							j=k;
						}
					}
					if (j>-1) {
						MYSQL_ROW row=mysql_fetch_row(mmsd->result);
						if (row) {
							repl_lag=-1;
							if (row[j]) { // if Seconds_Behind_Master is not NULL
								repl_lag=atoi(row[j]);
							}
						}
					}
					if (repl_lag>=0) {
						rc=sqlite3_bind_int64(statement, 5, repl_lag); assert(rc==SQLITE_OK);
					} else {
						rc=sqlite3_bind_null(statement, 5); assert(rc==SQLITE_OK);
					}
					mysql_free_result(mmsd->result);
					mmsd->result=NULL;
				} else {
					rc=sqlite3_bind_null(statement, 5); assert(rc==SQLITE_OK);
				}
				rc=sqlite3_bind_text(statement, 6, mmsd->mysql_error_msg, -1, SQLITE_TRANSIENT); assert(rc==SQLITE_OK);
				SAFE_SQLITE3_STEP(statement);
				rc=sqlite3_clear_bindings(statement); assert(rc==SQLITE_OK);
				rc=sqlite3_reset(statement); assert(rc==SQLITE_OK);
				//MyHGM->replication_lag_action(mmsd->hostgroup_id, mmsd->hostname, mmsd->port, (repl_lag==-1 ? 0 : repl_lag));
				MyHGM->replication_lag_action(mmsd->hostgroup_id, mmsd->hostname, mmsd->port, repl_lag);
				delete mmsd;
			}
			sqlite3_finalize(statement);
			free(sds);
		}

		if (resultset)
			delete resultset;

		event_base_free(libevent_base);


__sleep_monitor_replication_lag:
		t2=monotonic_time();
		if (t2<next_loop_at) {
			unsigned long long st=0;
			st=next_loop_at-t2;
			if (st > 500000) {
				st = 500000;
			}
			usleep(st);
		}
	}
	if (mysql_thr) {
		delete mysql_thr;
		mysql_thr=NULL;
	}
	return NULL;
}

void * MySQL_Monitor::run() {
	// initialize the MySQL Thread (note: this is not a real thread, just the structures associated with it)
	unsigned int MySQL_Monitor__thread_MySQL_Thread_Variables_version;
	MySQL_Thread * mysql_thr = new MySQL_Thread();
	mysql_thr->curtime=monotonic_time();
	MySQL_Monitor__thread_MySQL_Thread_Variables_version=GloMTH->get_global_version();
	mysql_thr->refresh_variables();
	std::thread * monitor_connect_thread = new std::thread(&MySQL_Monitor::monitor_connect,this);
	std::thread * monitor_ping_thread = new std::thread(&MySQL_Monitor::monitor_ping,this);
	std::thread * monitor_read_only_thread = new std::thread(&MySQL_Monitor::monitor_read_only,this);
//	std::thread * monitor_replication_lag_thread = new std::thread(&MySQL_Monitor::monitor_replication_lag,this);
	while (shutdown==false) {
		unsigned int glover=GloMTH->get_global_version();
		if (MySQL_Monitor__thread_MySQL_Thread_Variables_version < glover ) {
			MySQL_Monitor__thread_MySQL_Thread_Variables_version=glover;
			mysql_thr->refresh_variables();
			//proxy_error("%s\n","MySQL_Monitor refreshing variables");
			My_Conn_Pool->purge_missing_servers(NULL);
		}
		usleep(500000);
	}
	monitor_connect_thread->join();
	monitor_ping_thread->join();
	monitor_read_only_thread->join();
//	monitor_replication_lag_thread->join();
	if (mysql_thr) {
		delete mysql_thr;
		mysql_thr=NULL;
	}
	return NULL;
};



/*
MDB_ASYNC_ST MySQL_Monitor_State_Data::handler2(short event) {

handler_again:
  switch (async_state_machine) {
    case ASYNC_PING_START:
      ping_start();
      if (async_exit_status) {
        next_event(ASYNC_PING_CONT);
      } else {
        NEXT_IMMEDIATE2(ASYNC_PING_END);
      }
      break;
    case ASYNC_PING_CONT:
//      assert(myds->sess->status==PINGING_SERVER);
      if (event) {
        ping_cont(event);
      }
//      if (async_exit_status) {
//        if (myds->sess->thread->curtime >= myds->wait_until) {
//          NEXT_IMMEDIATE(ASYNC_PING_TIMEOUT);
//        } else {
//          next_event(ASYNC_PING_CONT);
//        }
//      } else {
//        NEXT_IMMEDIATE(ASYNC_PING_END);
//      }
      break;
    case ASYNC_PING_END:
      if (interr) {
        NEXT_IMMEDIATE2(ASYNC_PING_FAILED);
      } else {
        NEXT_IMMEDIATE2(ASYNC_PING_SUCCESSFUL);
      }
      break;
    case ASYNC_PING_SUCCESSFUL:
      break;
    case ASYNC_PING_FAILED:
      break;
    case ASYNC_PING_TIMEOUT:
      break;
		default:
			break;
		}
	return async_state_machine;
}

int MySQL_Monitor_State_Data::async_ping(short event) {
	PROXY_TRACE();
	assert(mysql);
	switch (async_state_machine) {
		case ASYNC_PING_SUCCESSFUL:
			async_state_machine=ASYNC_IDLE;
			return 0;
			break;
		case ASYNC_PING_FAILED:
			return -1;
			break;
		case ASYNC_PING_TIMEOUT:
			return -2;
			break;
		case ASYNC_IDLE:
			async_state_machine=ASYNC_PING_START;
		default:
			handler2(event);
			break;
	}

	// check again
	switch (async_state_machine) {
		case ASYNC_PING_SUCCESSFUL:
			async_state_machine=ASYNC_IDLE;
			return 0;
			break;
		case ASYNC_PING_FAILED:
			return -1;
			break;
		case ASYNC_PING_TIMEOUT:
			return -2;
			break;
		default:
			return 1;
			break;
	}
	return 1;
}

void MySQL_Monitor_State_Data::next_event(MDB_ASYNC_ST new_st) {
#ifdef DEBUG
  int fd;
#endif // DEBUG
  wait_events=0;

  if (async_exit_status & MYSQL_WAIT_READ)
    wait_events |= POLLIN;
  if (async_exit_status & MYSQL_WAIT_WRITE)
    wait_events|= POLLOUT;
  if (wait_events)
#ifdef DEBUG
    fd= mysql_get_socket(mysql);
#else
    mysql_get_socket(mysql);
#endif // DEBUG
  else
#ifdef DEBUG
    fd= -1;
#endif // DEBUG
  if (async_exit_status & MYSQL_WAIT_TIMEOUT) {
  timeout=10000;
  } else {
  }
  proxy_debug(PROXY_DEBUG_NET, 8, "fd=%d, wait_events=%d , old_ST=%d, new_ST=%d\n", fd, wait_events, async_state_machine, new_st);
  async_state_machine = new_st;
}

void MySQL_Monitor_State_Data::ping_start() {
  async_exit_status = mysql_ping_start(&interr,mysql);
}

void MySQL_Monitor_State_Data::ping_cont(short event) {
  async_exit_status = mysql_ping_cont(&interr,mysql, mysql_status2(event, true));
}
*/
