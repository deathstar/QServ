#include "game.h"
#include <GeoIP.h>
#include "sauerLua.h"
#include <stdio.h>
#include <time.h>
#include "IRCbot.h"

extern ENetAddress masteraddress; 
extern ircBot irc;
 
//fpsgame/server.cpp (C) 2012 QServ  

namespace game {void parseoptions(vector<const char *> &args) {
loopv(args)
#ifndef STANDALONE 
if(!game::clientoption(args[i]))
#endif 
if(!server::serveroption(args[i])) 
conoutf(CON_ERROR, "Command not found: %s", args[i]); 
}}

namespace server {
 struct server_entity //Server side version of "entity" type
 {
 int type;
 int spawntime;
 char spawned;
 };

 static const int DEATHMILLIS = 300;

 struct clientinfo;
 struct gameevent
 {
 virtual ~gameevent() {}

 virtual bool flush(clientinfo *ci, int fmillis);
 virtual void process(clientinfo *ci) {}

 virtual bool keepable() const { return false; }
 };

 struct timedevent : gameevent
 {
 int millis;

 bool flush(clientinfo *ci, int fmillis);
 };

 struct hitinfo
 {
 int target;
 int lifesequence;
 int rays;
 float dist;
 vec dir;
 };

 struct shotevent : timedevent
 {
 int id, gun;
 vec from, to;
 vector<hitinfo> hits;

 void process(clientinfo *ci);
 };

 struct explodeevent : timedevent
 {
 int id, gun;
 vector<hitinfo> hits;

 bool keepable() const { return true; }
 void process(clientinfo *ci);
 };

 struct suicideevent : gameevent
 {
 void process(clientinfo *ci);
 };

 struct pickupevent : gameevent
 {
 int ent;
 void process(clientinfo *ci);
 };

 template <int N>
 struct projectilestate
 {
 int projs[N];
 int numprojs;

 projectilestate() : numprojs(0) {}

 void reset() { numprojs = 0; }
 void add(int val)
 {
 if(numprojs>=N) numprojs = 0;
 projs[numprojs++] = val;
 }

 bool remove(int val)
 {
 loopi(numprojs) if(projs[i]==val)
 {
 projs[i] = projs[--numprojs];
 return true;
 }
 return false;
 }
 };

 struct gamestate : fpsstate
 {
 vec o;
 int state, editstate;
 int lastdeath, lastspawn, lifesequence;
 int lastshot;
 projectilestate<8> rockets, grenades;
 int frags, flags, deaths, teamkills, shotdamage, damage;
 int lasttimeplayed, timeplayed;
 float effectiveness;

int64_t lastfragmillis;
int multifrags;
int spreefrags;

 gamestate() : state(CS_DEAD), editstate(CS_DEAD) {}

 bool isalive(int gamemillis)
 {
 return state==CS_ALIVE || (state==CS_DEAD && gamemillis - lastdeath <= DEATHMILLIS);
 }

 bool waitexpired(int gamemillis)
 {
 return gamemillis - lastshot >= gunwait;
 }

 void reset()//reset rockets/health and respawn before clearing int's
 { 
 if(state!=CS_SPECTATOR) state = editstate = CS_DEAD;
 respawn();
 rockets.reset();
 grenades.reset();
 maxhealth = 100;
 frags = flags = deaths = teamkills = shotdamage = damage = 0;
 timeplayed = 0;
 effectiveness = 0;
 lastfragmillis = 0;
 multifrags = spreefrags = 0;
 }

 void respawn()
 {
 fpsstate::respawn();
 o = vec(-1e10f, -1e10f, -1e10f);
 lastshot = 0;
 lastdeath = 0;
 lastspawn = -1;
 }

 void reassign()
 {
 respawn();
 rockets.reset();
 grenades.reset();
 }
 };

 struct savedscore
 {
 uint ip;
 string name;
 int maxhealth, frags, flags, deaths, teamkills, shotdamage, damage;
 int timeplayed;
 float effectiveness;

 void save(gamestate &gs)
 {
 maxhealth = gs.maxhealth;
 frags = gs.frags;
 flags = gs.flags;
 deaths = gs.deaths;
 teamkills = gs.teamkills;
 shotdamage = gs.shotdamage;
 damage = gs.damage;
 timeplayed = gs.timeplayed;
 effectiveness = gs.effectiveness;
 out(ECHO_CONSOLE, "Client statistics saved...");
 }

 void restore(gamestate &gs)
 {
 if(gs.health==gs.maxhealth) gs.health = maxhealth;
 gs.maxhealth = maxhealth;
 gs.frags = frags;
 gs.flags = flags;
 gs.deaths = deaths;
 gs.teamkills = teamkills;
 gs.shotdamage = shotdamage;
 gs.damage = damage;
 gs.timeplayed = timeplayed;
 gs.effectiveness = effectiveness;
 out(ECHO_SERV, "Client reconnected, lagged out, or connected another client");
 out(ECHO_CONSOLE, "Client reconnected, lagged out, or added another client; found previous score.");
 }
 };

 extern int gamemillis, nextexceeded;

 struct clientinfo
 {
 int clientnum, ownernum, connectmillis, sessionid, overflow, connectedmillis;
 string name, team, mapvote;
 char *ip; //ipstring
 bool connected, local, timesync;
 int privilege;
 int playermodel;
 int modevote;
 int gameoffset, lastevent, pushed, exceeded;
 gamestate state;
 vector<gameevent *> events;
 vector<uchar> position, messages;
 int posoff, poslen, msgoff, msglen;
 vector<clientinfo *> bots;
 uint authreq;
 string authname;
 int ping, aireinit;
 string clientmap;
 int mapcrc;
 bool warned, gameclip, pingwarned;
 ENetPacket *clipboard;
 int lastclipboard, needclipboard;

 clientinfo() : clipboard(NULL) { reset(); }
 ~clientinfo() { events.deletecontents(); cleanclipboard(); }

 void addevent(gameevent *e)
 {
 if(state.state==CS_SPECTATOR || events.length()>100) delete e;
 else events.add(e);
 }

 enum
 {
 PUSHMILLIS = 2500
 };
 bool checkpushed(int millis, int range)
 {
 return millis >= pushed - range && millis <= pushed + range;
 }

 int calcpushrange()
 {
 ENetPeer *peer = getclientpeer(ownernum);
 return PUSHMILLIS + (peer ? peer->roundTripTime + peer->roundTripTimeVariance : ENET_PEER_DEFAULT_ROUND_TRIP_TIME);
 }

 void scheduleexceeded()
 {
 if(state.state!=CS_ALIVE || !exceeded) return;
 int range = calcpushrange();
 if(!nextexceeded || exceeded + range < nextexceeded) nextexceeded = exceeded + range;
 }

void setpushed()
 {
 pushed = max(pushed, gamemillis);
 if(exceeded && checkpushed(exceeded, calcpushrange())) exceeded = 0;
 }

 void setexceeded()
 {
 if(state.state==CS_ALIVE && !exceeded && !checkpushed(gamemillis, calcpushrange())) exceeded = gamemillis;
 scheduleexceeded();
 }


 bool checkexceeded()
 {
 return state.state==CS_ALIVE && exceeded && gamemillis > exceeded + calcpushrange();
 }

 void reset()
 {
 name[0] = team[0] = 0;
 playermodel = -1;
 privilege = PRIV_NONE;
 connected = local = false;
 authreq = 0;
 position.setsize(0);
 messages.setsize(0);
 ping = 0;
 aireinit = 0;
 needclipboard = 0;
 cleanclipboard();
 mapchange();
 }

 void mapchange()
 {
 mapvote[0] = 0;
 state.reset();
 events.deletecontents();
 overflow = 0;
 timesync = false;
 lastevent = 0;
 exceeded = 0;
 pushed = 0;
 clientmap[0] = '\0';
 mapcrc = 0;
 warned = false;
 gameclip = false;
 }

 void reassign()
 {
 state.reassign();
 events.deletecontents();
 timesync = false;
 lastevent = 0;
 }

 void cleanclipboard(bool fullclean = true)
 {
 if(clipboard) { if(--clipboard->referenceCount <= 0) enet_packet_destroy(clipboard); clipboard = NULL; }
 if(fullclean) lastclipboard = 0;
 }

 int geteventmillis(int servmillis, int clientmillis)
 {
 if(!timesync || (events.empty() && state.waitexpired(servmillis)))
 {
 timesync = true;
 gameoffset = servmillis - clientmillis;
 return servmillis;
 }
 else return gameoffset + clientmillis;
 }
 };

 struct worldstate
 {
 int uses;
 vector<uchar> positions, messages;
 };

 struct ban
 {
 int time;
 uint ip;
 };

 namespace aiman
 {
 extern void removeai(clientinfo *ci);
 extern void clearai();
 extern void checkai();
 extern void reqadd(clientinfo *ci, int skill);
 extern void reqdel(clientinfo *ci);
 extern void setbotlimit(clientinfo *ci, int limit);
 extern void setbotbalance(clientinfo *ci, bool balance);
 extern void changemap();
 extern void addclient(clientinfo *ci);
 extern void changeteam(clientinfo *ci);
 }

 #define MM_MODE 0xF
 #define MM_AUTOAPPROVE 0x1000
 #define MM_PRIVSERV (MM_MODE | MM_AUTOAPPROVE)
 #define MM_PUBSERV ((1<<MM_OPEN) | (1<<MM_VETO))
 #define MM_COOPSERV (MM_AUTOAPPROVE | MM_PUBSERV | (1<<MM_LOCKED))

 bool notgotitems = true; //True when map has changed and waiting for clients to send item
 int gamemode = 0;
 int gamemillis = 0, gamelimit = 0, nextexceeded = 0;
 string smapname = ""; //mapname (map name)
 bool mapreload = false;
 bool gamepaused = false; 
 bool persist = false;
 int interm = 0;
 stream *mapdata = NULL;
 enet_uint32 lastsend = 0;
 int mastermode = MM_OPEN, mastermask = MM_PRIVSERV;
 int currentmaster = -1;
 vector<uint> allowedips;
 vector<ban> bannedips;
 vector<clientinfo *> connects, clients, bots;
 vector<worldstate *> worldstates;
 bool reliablemessages = false;
    
 void clearbans() {
        bannedips.shrink(0);
        out(ECHO_SERV, "Server bans \f0cleared");
        out(ECHO_CONSOLE, "Server bans cleared");
        out(ECHO_IRC, "All bans cleared");
    }
    
 void banPlayer(int i)
 {
 ban &b = bannedips.add();
 b.time = totalmillis;
 b.ip = getclientip(i);
 allowedips.removeobj(b.ip);
 disconnect_client(i, DISC_BANNED);
 }
 struct demofile
 {
 string info;
 uchar *data;
 int len;
 };

 #define MAXDEMOS 12
 vector<demofile> demos;
 bool demonextmatch = false;
 stream *demotmp = NULL, *demorecord = NULL, *demoplayback = NULL;
 int nextplayback = 0, demomillis = 0;

//QServ Variables & Boolans
VAR(chattoconsole, 0, 0, 1);
VAR(enablegeoip, 0, 0, 1);
VAR(tkpenalty, 0, 0, 1);
VAR(minspreefrags, 2, 5, INT_MAX); 
VAR(shotguninsta, 0, 0, 1);
VAR(rocketinsta, 0, 0, 1);
VAR(chainsawinsta, 0, 0, 1);
VAR(enablestopservercmd, 0, 0, 1);
VAR(conteleport, 0, 0, 1);
SVAR(serverdesc, ""); 
SVAR(welcomemsg, "");
SVAR(tkmsg, "");
SVAR(adminpass, "");
SVAR(swaretext, "");
SVAR(botname, "");
SVAR(codelastupdated, "");
SVAR(qservversion, "");
SVAR(operators, "");
SVAR(pingwarnmsg, "");
SVAR(callopmsg, ""); 
SVAR(spreesuicidemsg, "");
SVAR(spreefinmsg, "");
SVAR(serverpass, "");
VARF(publicserver, 0, 0, 2, {
switch(publicserver)
{
case 0: default: mastermask = MM_PRIVSERV; break;
case 1: mastermask = MM_PUBSERV; break;
case 2: mastermask = MM_COOPSERV; break;
}
});
void sendservmsg(const char *s) { sendf(-1, 1, "ris", N_SERVMSG, s); }

bool firstblood = false;

 void *newclientinfo() { return new clientinfo; }
 void deleteclientinfo(void *ci) { delete (clientinfo *)ci; }

 clientinfo *getinfo(int n)
 {
 if(n < MAXCLIENTS) return (clientinfo *)getclientinfo(n);
 n -= MAXCLIENTS;
 return bots.inrange(n) ? bots[n] : NULL;
 }

 vector<server_entity> sents;
 vector<savedscore> scores;

 int msgsizelookup(int msg)
 {
 static int sizetable[NUMSV] = { -1 };
 if(sizetable[0] < 0)
 {
 memset(sizetable, -1, sizeof(sizetable));
 for(const int *p = msgsizes; *p >= 0; p += 2) sizetable[p[0]] = p[1];
 }
 return msg >= 0 && msg < NUMSV ? sizetable[msg] : -1;
 }

 const char *modename(int n, const char *unknown)
 {
 if(m_valid(n)) return gamemodes[n - STARTGAMEMODE].name;
 return unknown;
 }

 const char *mastermodename(int n, const char *unknown)
 {
 return (n>=MM_START && size_t(n-MM_START)<sizeof(mastermodenames)/sizeof(mastermodenames[0])) ? mastermodenames[n-MM_START] : unknown;
 }

 const char *privname(int type)
 {
 switch(type)
 {
 case PRIV_ADMIN: return "admin";
 case PRIV_MASTER: return "master";
 default: return "unknown";
 }
 }

 void resetitems()
 {
 sents.shrink(0);
 //cps.reset();
 }

bool serveroption(const char *arg)
 {
 if(arg[0]=='-') switch(arg[1])
 {
 case 'n': setsvar("serverdesc", &arg[2]); return true;
 case 'y': setsvar("serverpass", &arg[2]); return true;
 case 'p': setsvar("adminpass", &arg[2]); return true;
 case 'o': setvar("publicserver", atoi(&arg[2])); return true;
 case 'g': setvar("serverbotlimit", atoi(&arg[2])); return true;
 }
 return false;
 }
void startserv() //start server
{
char *servername = serverdesc;
char *passwrd = adminpass; 
if(!strcmp(servername, "QServ 10 Viper")) {printf("\33[31mYour Servername is defualt, please configure it in \"server-init.cfg\"\33[0m\n");}
if(!strcmp(passwrd, "changeme")) {printf("\33[31mYour Admin Password is defualt, please configure it in \"server-init.cfg\"\33[0m\n");}
printf("\33[34m\"%s\" with Admin Password \"%s\" started. Listening on port %i\33[0m\n\n", servername, passwrd, getvar("serverport"));
time_t rawtime;
struct tm * timeinfo;
char tad [80];
time ( &rawtime );
timeinfo = localtime ( &rawtime );
//time format (hour:minute am/pm timezone dayname month day year)
strftime (tad,80,"Server started at %I:%M%p %Z %A, %B %d, %Y",timeinfo); 
puts(tad); 
printf("\33[31mCtrl-C to stop server\33[0m\n");

}

 int getmastercn()
 {
 loopv(clients)
 {
 clientinfo *ci = clients[i];
 if(ci->privilege == PRIV_MASTER)
 return ci->clientnum;
 }
 return -1;
 }

string blkmsg[3] = {"fuck", "shit", "cunt"};
char textcmd(const char *a, const char *text){
for (int b=0; b<strlen(a); b++) {
if (text[b+1] != a[b]) {
return false;
}
}
if(text[strlen(a)+1] != ' ' && text[strlen(a)+1] != '\0') {
for (int l=0; l<3; l++) {
if (strcmp(blkmsg[l], a)) {
return true;
break;
}
}
return false;
}
return true;
}
void textblk(const char *b, char *text, clientinfo *ci){
bool bad=false;
for (int a=0; a<strlen(text); a++) {
if(textcmd(b, text+a-1)) bad=true;
}
if(bad){
int n = rand() % 7 + 0;
defformatstring(d)("\f%i%s \f0%s!", n, swaretext, ci->name);
sendservmsg(d);
bad=false;
}
}

 void serverinit()
 {
 smapname[0] = '\0';
 resetitems();
 }

 int numclients(int exclude = -1, bool nospec = true, bool noai = true, bool priv = false)
 {
 int n = 0;
 loopv(clients)
 {
 clientinfo *ci = clients[i];
 if(ci->clientnum!=exclude && (!nospec || ci->state.state!=CS_SPECTATOR || (priv && (ci->privilege || ci->local))) && (!noai || ci->state.aitype == AI_NONE)) n++;
 }
 return n;
 }

 bool duplicatename(clientinfo *ci, char *name)
 {
 if(!name) name = ci->name;
 loopv(clients) if(clients[i]!=ci && !strcmp(name, clients[i]->name)) return true;
 return false;
 }

 const char *colorname(clientinfo *ci, char *name = NULL)
 {
 if(!name) name = ci->name;
 if(name[0] && !duplicatename(ci, name) && ci->state.aitype == AI_NONE) return name;
 static string cname[3];
 static int cidx = 0;
 cidx = (cidx+1)%3;
 formatstring(cname[cidx])(ci->state.aitype == AI_NONE ? "%s \fs\f5(%d)\fr" : "%s \fs\f5[%d]\fr", name, ci->clientnum);
 return cname[cidx];
 }

 struct servmode
 {
 virtual ~servmode() {}

 virtual void entergame(clientinfo *ci) {}
 virtual void leavegame(clientinfo *ci, bool disconnecting = false) {}

 virtual void moved(clientinfo *ci, const vec &oldpos, bool oldclip, const vec &newpos, bool newclip) {}
 virtual bool canspawn(clientinfo *ci, bool connecting = false) { return true; }
 virtual void spawned(clientinfo *ci) {}
 virtual int fragvalue(clientinfo *victim, clientinfo *actor)
 {
 if(victim==actor || isteam(victim->team, actor->team)) return -1;
 return 1;
 }
 virtual void died(clientinfo *victim, clientinfo *actor) {}
 virtual bool canchangeteam(clientinfo *ci, const char *oldteam, const char *newteam) { return true; }
 virtual void changeteam(clientinfo *ci, const char *oldteam, const char *newteam) {}
 virtual void initclient(clientinfo *ci, packetbuf &p, bool connecting) {}
 virtual void update() {}
 virtual void reset(bool empty) {}
 virtual void intermission() {}
 virtual bool hidefrags() { return false; }
 virtual int getteamscore(const char *team) { return 0; }
 virtual void getteamscores(vector<teamscore> &scores) {}
 virtual bool extinfoteam(const char *team, ucharbuf &p) { return false; }
 };

 #define SERVMODE 1
 #include "capture.h"
 #include "ctf.h"

 captureservmode capturemode;
 ctfservmode ctfmode;
 servmode *smode = NULL;

 bool canspawnitem(int type) { return !m_noitems && (type>=I_SHELLS && type<=I_QUAD && (!m_noammo || type<I_SHELLS || type>I_CARTRIDGES)); }

 int spawntime(int type)
 {
 if(m_classicsp) return INT_MAX;
 int np = numclients(-1, true, false);
 np = np<3 ? 4 : (np>4 ? 2 : 3); // spawn times are dependent on number of players
 int sec = 0;
 switch(type)
 {
 case I_SHELLS:
 case I_BULLETS:
 case I_ROCKETS:
 case I_ROUNDS:
 case I_GRENADES:
 case I_CARTRIDGES: sec = np*4; break;
 case I_HEALTH: sec = np*5; break;
 case I_GREENARMOUR:
 case I_YELLOWARMOUR: sec = 20; break;
 case I_BOOST:
 case I_QUAD: sec = 40+rnd(40); break;
 }
 return sec*1000;
 }

 bool delayspawn(int type)
 {
 switch(type)
 {
 case I_GREENARMOUR:
 case I_YELLOWARMOUR:
 return !m_classicsp;
 case I_BOOST:
 case I_QUAD:
 return true;
 default:
 return false;
 }
 }

 bool pickup(int i, int sender) // server side item pickup, acknowledge first client that gets it
 {
 if((m_timed && gamemillis>=gamelimit) || !sents.inrange(i) || !sents[i].spawned) return false;
 clientinfo *ci = getinfo(sender);
 if(!ci || (!ci->local && !ci->state.canpickup(sents[i].type))) return false;
 sents[i].spawned = false;
 sents[i].spawntime = spawntime(sents[i].type);
 sendf(-1, 1, "ri3", N_ITEMACC, i, sender);
 ci->state.pickup(sents[i].type);
 return true;
 }

 clientinfo *choosebestclient(float &bestrank)
 {
 clientinfo *best = NULL;
 bestrank = -1;
 loopv(clients)
 {
 clientinfo *ci = clients[i];
 if(ci->state.timeplayed<0) continue;
 float rank = ci->state.state!=CS_SPECTATOR ? ci->state.effectiveness/max(ci->state.timeplayed, 1) : -1;
 if(!best || rank > bestrank) { best = ci; bestrank = rank; }
 }
 return best;
 }

 void autoteam()
 {
 static const char *teamnames[2] = {"good", "evil"};
 vector<clientinfo *> team[2];
 float teamrank[2] = {0, 0};
 for(int round = 0, remaining = clients.length(); remaining>=0; round++)
 {
 int first = round&1, second = (round+1)&1, selected = 0;
 while(teamrank[first] <= teamrank[second])
 {
 float rank;
 clientinfo *ci = choosebestclient(rank);
 if(!ci) break;
 if(smode && smode->hidefrags()) rank = 1;
 else if(selected && rank<=0) break;
 ci->state.timeplayed = -1;
 team[first].add(ci);
 if(rank>0) teamrank[first] += rank;
 selected++;
 if(rank<=0) break;
 }
 if(!selected) break;
 remaining -= selected;
 }
 loopi(sizeof(team)/sizeof(team[0]))
 {
 loopvj(team[i])
 {
 clientinfo *ci = team[i][j];
 if(!strcmp(ci->team, teamnames[i])) continue;
 copystring(ci->team, teamnames[i], MAXTEAMLEN+1);
 sendf(-1, 1, "riisi", N_SETTEAM, ci->clientnum, teamnames[i], -1);
 }
 }
 }

 struct teamrank
 {
 const char *name;
 float rank;
 int clients;

 teamrank(const char *name) : name(name), rank(0), clients(0) {}
 };

 const char *chooseworstteam(const char *suggest = NULL, clientinfo *exclude = NULL)
 {
 teamrank teamranks[2] = { teamrank("good"), teamrank("evil") };
 const int numteams = sizeof(teamranks)/sizeof(teamranks[0]);
 loopv(clients)
 {
 clientinfo *ci = clients[i];
 if(ci==exclude || ci->state.aitype!=AI_NONE || ci->state.state==CS_SPECTATOR || !ci->team[0]) continue;
 ci->state.timeplayed += lastmillis - ci->state.lasttimeplayed;
 ci->state.lasttimeplayed = lastmillis;

 loopj(numteams) if(!strcmp(ci->team, teamranks[j].name))
 {
 teamrank &ts = teamranks[j];
 ts.rank += ci->state.effectiveness/max(ci->state.timeplayed, 1);
 ts.clients++;
 break;
 }
 }
 teamrank *worst = &teamranks[numteams-1];
 loopi(numteams-1)
 {
 teamrank &ts = teamranks[i];
 if(smode && smode->hidefrags())
 {
 if(ts.clients < worst->clients || (ts.clients == worst->clients && ts.rank < worst->rank)) worst = &ts;
 }
 else if(ts.rank < worst->rank || (ts.rank == worst->rank && ts.clients < worst->clients)) worst = &ts;
 }
 return worst->name;
 }

 void writedemo(int chan, void *data, int len)
 {
 if(!demorecord) return;
 int stamp[3] = { gamemillis, chan, len };
 lilswap(stamp, 3);
 demorecord->write(stamp, sizeof(stamp));
 demorecord->write(data, len);
 }

 void recordpacket(int chan, void *data, int len)
 {
 writedemo(chan, data, len);
 }

 void enddemorecord()
 {
 if(!demorecord) return;

 DELETEP(demorecord);

 if(!demotmp) return;

 int len = demotmp->size();
 if(demos.length()>=MAXDEMOS)
 {
 delete[] demos[0].data;
 demos.remove(0);
 }
 demofile &d = demos.add();
 time_t t = time(NULL);
 char *timestr = ctime(&t), *trim = timestr + strlen(timestr);
 while(trim>timestr && isspace(*--trim)) *trim = '\0';
 d.data = new uchar[len];
     formatstring(d.info)("%s: %s, %s, %.2f%s", timestr, modename(gamemode), smapname, len > 1024*1024 ? len/(1024*1024.f) : len/1024.0f, len > 1024*1024 ? "MB" : "kB");
     defformatstring(msg)("Demo \"%s\" succesfully recorded", d.info);
     sendservmsg(msg);
 d.len = len;
 demotmp->seek(0, SEEK_SET);
 demotmp->read(d.data, len);
 DELETEP(demotmp);
 }

 int welcomepacket(packetbuf &p, clientinfo *ci);
 void sendwelcome(clientinfo *ci);

 void setupdemorecord()
 {
 if(!m_mp(gamemode) || m_edit) return;

 demotmp = opentempfile("demorecord", "w+b");
 if(!demotmp) return;

 stream *f = opengzfile(NULL, "wb", demotmp);
 if(!f) { DELETEP(demotmp); return; }

 sendservmsg("\f4Recording demo...");

 demorecord = f;

 demoheader hdr;
 memcpy(hdr.magic, DEMO_MAGIC, sizeof(hdr.magic));
 hdr.version = DEMO_VERSION;
 hdr.protocol = PROTOCOL_VERSION;
 lilswap(&hdr.version, 2);
 demorecord->write(&hdr, sizeof(demoheader));

 packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
 welcomepacket(p, NULL);
 writedemo(1, p.buf, p.len);
 }

 void listdemos(int cn)
 {
 packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
 putint(p, N_SENDDEMOLIST);
 putint(p, demos.length());
 loopv(demos) sendstring(demos[i].info, p);
 sendpacket(cn, 1, p.finalize());
 }

 void cleardemos(int n)
 {
 if(!n)
 {
 loopv(demos) delete[] demos[i].data;
 demos.shrink(0);
 sendservmsg("\f1Notice: \f7All demos were deleted from the server");
 }
 else if(demos.inrange(n-1))
 {
 delete[] demos[n-1].data;
 demos.remove(n-1);
 defformatstring(msg)("Deleted demo: %d", n);
 sendservmsg(msg);
 }
 }

 void senddemo(int cn, int num)
 {
 if(!num) num = demos.length();
 if(!demos.inrange(num-1)) return;
 demofile &d = demos[num-1];
 sendf(cn, 2, "rim", N_SENDDEMO, d.len, d.data);
 }

 void enddemoplayback()
 {
 if(!demoplayback) return;
 DELETEP(demoplayback);

 loopv(clients) sendf(clients[i]->clientnum, 1, "ri3", N_DEMOPLAYBACK, 0, clients[i]->clientnum);

 sendservmsg("Demo has finished playing");

 loopv(clients) sendwelcome(clients[i]);
 }

 void setupdemoplayback()
 {
 if(demoplayback) return;
 demoheader hdr;
 string msg;
 msg[0] = '\0';
 defformatstring(file)("%s.dmo", smapname);
 demoplayback = opengzfile(file, "rb");
 if(!demoplayback) formatstring(msg)("\f3Error: \f7Could not play demo \"%s\"", file);
 else if(demoplayback->read(&hdr, sizeof(demoheader))!=sizeof(demoheader) || memcmp(hdr.magic, DEMO_MAGIC, sizeof(hdr.magic)))
 formatstring(msg)("\f3Error: \f7\"%s\" is not a demo file", file);
 else
 {
 lilswap(&hdr.version, 2);
 if(hdr.version!=DEMO_VERSION) formatstring(msg)("Demo \"%s\" requires an %s ve=rsion of Cube 2: Sauerbraten", file, hdr.version<DEMO_VERSION ? "older" : "newer");
 else if(hdr.protocol!=PROTOCOL_VERSION) formatstring(msg)("Demo \"%s\" requires an %s version of Cube 2: Sauerbraten", file, hdr.protocol<PROTOCOL_VERSION ? "older" : "newer");
 }
 if(msg[0])
 {
 DELETEP(demoplayback);
 sendservmsg(msg);
 return;
 }

 formatstring(msg)("Playing demo \"%s\"", file);
 sendservmsg(msg);

 demomillis = 0;
 sendf(-1, 1, "ri3", N_DEMOPLAYBACK, 1, -1);

 if(demoplayback->read(&nextplayback, sizeof(nextplayback))!=sizeof(nextplayback))
 {
 enddemoplayback();
 return;
 }
 lilswap(&nextplayback, 1);
 }

 void readdemo()
 {
 if(!demoplayback || gamepaused) return;
 demomillis += curtime;
 while(demomillis>=nextplayback)
 {
 int chan, len;
 if(demoplayback->read(&chan, sizeof(chan))!=sizeof(chan) ||
 demoplayback->read(&len, sizeof(len))!=sizeof(len))
 {
 enddemoplayback();
 return;
 }
 lilswap(&chan, 1);
 lilswap(&len, 1);
 ENetPacket *packet = enet_packet_create(NULL, len, 0);
 if(!packet || demoplayback->read(packet->data, len)!=len)
 {
 if(packet) enet_packet_destroy(packet);
 enddemoplayback();
 return;
 }
 sendpacket(-1, chan, packet);
 if(!packet->referenceCount) enet_packet_destroy(packet);
 if(demoplayback->read(&nextplayback, sizeof(nextplayback))!=sizeof(nextplayback))
 {
 enddemoplayback();
 return;
 }
 lilswap(&nextplayback, 1);
 }
 }

 void stopdemo()
 {
 if(m_demo) enddemoplayback();
 else enddemorecord();
 }

 void pausegame(bool val)
 {
 if(gamepaused==val) return;
 gamepaused = val;
 sendf(-1, 1, "rii", N_PAUSEGAME, gamepaused ? 1 : 0);
 }

 void hashpassword(int cn, int sessionid, const char *pwd, char *result, int maxlen)
 {
 char buf[2*sizeof(string)];
 formatstring(buf)("%d %d ", cn, sessionid);
 copystring(&buf[strlen(buf)], pwd);
 if(!hashstring(buf, result, maxlen)) *result = '\0';
 }

 bool checkpassword(clientinfo *ci, const char *wanted, const char *given)
 {
 string hash;
 hashpassword(ci->clientnum, ci->sessionid, wanted, hash, sizeof(hash));
 return !strcmp(hash, given);
 }

 void revokemaster(clientinfo *ci)
 {
 ci->privilege = PRIV_NONE;
 if(ci->state.state==CS_SPECTATOR && !ci->local) aiman::removeai(ci);
 }

 void setmaster(clientinfo *ci, bool val, const char *pass = "", const char *authname = NULL)
 {
 if(authname && !val) return;
 const char *name = "";
 if(val)
 {
 bool haspass = adminpass[0] && checkpassword(ci, adminpass, pass);
 if(ci->privilege)
 {
 if(!adminpass[0] || haspass==(ci->privilege==PRIV_ADMIN)) return;
 }
 else if(ci->state.state==CS_SPECTATOR && !haspass && !authname && !ci->local) return;
 loopv(clients) if(ci!=clients[i] && clients[i]->privilege)
 {
 if(haspass) clients[i]->privilege = PRIV_NONE;
 else if((authname || ci->local) && clients[i]->privilege<=PRIV_MASTER) continue;
 else return;
 }
 if(haspass) ci->privilege = PRIV_ADMIN;
 else if(!authname && !(mastermask&MM_AUTOAPPROVE) && !ci->privilege && !ci->local)
 {
 sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f3Error: \f7Unable to grant \f0Master \f7(It is currently disabled) \n\f5Authkey \f7holders, use \f1\"/auth\" \f7to obtain master.");
 return;
 }
 else
 {
 if(authname)
 {
 loopv(clients) if(ci!=clients[i] && clients[i]->privilege<=PRIV_MASTER) revokemaster(clients[i]);
 }
 ci->privilege = PRIV_MASTER;
 }
 name = privname(ci->privilege);
 }
 else
 {
 if(!ci->privilege) return;
 name = privname(ci->privilege);
 revokemaster(ci);
 }
 mastermode = MM_OPEN;
 allowedips.shrink(0);
 string msg;
string msg_irc;
if(val && authname) formatstring(msg)("\f0%s \f7claimed \f1%s \f7as '\fs\f5%s\fr'", colorname(ci), name, authname); //auth setmaster
 else formatstring(msg)("\f0%s \f7%s \f4%s", colorname(ci), val ? "claimed" : "relinquished", name);
 sendservmsg(msg);
formatstring(msg_irc)("%s %s %s", colorname(ci), val ? "claimed" : "relinquished", name);
irc.speak(msg_irc);
 currentmaster = val ? ci->clientnum : -1;
 sendf(-1, 1, "ri4", N_CURRENTMASTER, currentmaster, currentmaster >= 0 ? ci->privilege : 0, mastermode);
 if(gamepaused)
 {
 int admins = 0;
 loopv(clients) if(clients[i]->privilege >= PRIV_ADMIN || clients[i]->local) admins++;
 if(!admins) pausegame(false);
 }
 }

 savedscore &findscore(clientinfo *ci, bool insert)
 {
 uint ip = getclientip(ci->clientnum);
 if(!ip && !ci->local) return *(savedscore *)0;
 if(!insert)
 {
 loopv(clients)
 {
 clientinfo *oi = clients[i];
 if(oi->clientnum != ci->clientnum && getclientip(oi->clientnum) == ip && !strcmp(oi->name, ci->name))
 {
 oi->state.timeplayed += lastmillis - oi->state.lasttimeplayed;
 oi->state.lasttimeplayed = lastmillis;
 static savedscore curscore;
 curscore.save(oi->state);
 return curscore;
 }
 }
 }
 loopv(scores)
 {
 savedscore &sc = scores[i];
 if(sc.ip == ip && !strcmp(sc.name, ci->name)) return sc;
 }
 if(!insert) return *(savedscore *)0;
 savedscore &sc = scores.add();
 sc.ip = ip;
 copystring(sc.name, ci->name);
 return sc;
 }

 void savescore(clientinfo *ci)
 {
 savedscore &sc = findscore(ci, true);
 if(&sc) sc.save(ci->state);
 }

 int checktype(int type, clientinfo *ci)
 {
 if(ci && ci->local) return type;
 // only allow edit messages in coop-edit mode
 if(type>=N_EDITENT && type<=N_EDITVAR && !m_edit) return -1;
 // server only messages
 static const int servtypes[] = { N_SERVINFO, N_INITCLIENT, N_WELCOME, N_MAPRELOAD, N_SERVMSG, N_DAMAGE, N_HITPUSH, N_SHOTFX, N_EXPLODEFX, N_DIED, N_SPAWNSTATE, N_FORCEDEATH, N_ITEMACC, N_ITEMSPAWN, N_TIMEUP, N_CDIS, N_CURRENTMASTER, N_PONG, N_RESUME, N_BASESCORE, N_BASEINFO, N_BASEREGEN, N_ANNOUNCE, N_SENDDEMOLIST, N_SENDDEMO, N_DEMOPLAYBACK, N_SENDMAP, N_DROPFLAG, N_SCOREFLAG, N_RETURNFLAG, N_RESETFLAG, N_INVISFLAG, N_CLIENT, N_AUTHCHAL, N_INITAI };
 if(ci)
 {
 loopi(sizeof(servtypes)/sizeof(int)) if(type == servtypes[i]) return -1;
 if(type < N_EDITENT || type > N_EDITVAR || !m_edit)
 {
 if(type != N_POS && ++ci->overflow >= 200) return -2;
 }
 }
 return type;
 }

 void cleanworldstate(ENetPacket *packet)
 {
 loopv(worldstates)
 {
 worldstate *ws = worldstates[i];
 if(ws->positions.inbuf(packet->data) || ws->messages.inbuf(packet->data)) ws->uses--;
 else continue;
 if(!ws->uses)
 {
 delete ws;
 worldstates.remove(i);
 }
 break;
 }
 }

 void flushclientposition(clientinfo &ci)
 {
 if(ci.position.empty() || (!hasnonlocalclients() && !demorecord)) return;
 packetbuf p(ci.position.length(), 0);
 p.put(ci.position.getbuf(), ci.position.length());
 ci.position.setsize(0);
 sendpacket(-1, 0, p.finalize(), ci.ownernum);
 }

 void addclientstate(worldstate &ws, clientinfo &ci)
 {
 if(ci.position.empty()) ci.posoff = -1;
 else
 {
 ci.posoff = ws.positions.length();
 ws.positions.put(ci.position.getbuf(), ci.position.length());
 ci.poslen = ws.positions.length() - ci.posoff;
 ci.position.setsize(0);
 }
 if(ci.messages.empty()) ci.msgoff = -1;
 else
 {
 ci.msgoff = ws.messages.length();
 putint(ws.messages, N_CLIENT);
 putint(ws.messages, ci.clientnum);
 putuint(ws.messages, ci.messages.length());
 ws.messages.put(ci.messages.getbuf(), ci.messages.length());
 ci.msglen = ws.messages.length() - ci.msgoff;
 ci.messages.setsize(0);
 }
 }

 bool buildworldstate()
 {
 worldstate &ws = *new worldstate;
 loopv(clients)
 {
 clientinfo &ci = *clients[i];
 if(ci.state.aitype != AI_NONE) continue;
 ci.overflow = 0;
 addclientstate(ws, ci);
 loopv(ci.bots)
 {
 clientinfo &bi = *ci.bots[i];
 addclientstate(ws, bi);
 if(bi.posoff >= 0)
 {
 if(ci.posoff < 0) { ci.posoff = bi.posoff; ci.poslen = bi.poslen; }
 else ci.poslen += bi.poslen;
 }
 if(bi.msgoff >= 0)
 {
 if(ci.msgoff < 0) { ci.msgoff = bi.msgoff; ci.msglen = bi.msglen; }
 else ci.msglen += bi.msglen;
 }
 }
 }
 int psize = ws.positions.length(), msize = ws.messages.length();
 if(psize)
 {
 recordpacket(0, ws.positions.getbuf(), psize);
 ucharbuf p = ws.positions.reserve(psize);
 p.put(ws.positions.getbuf(), psize);
 ws.positions.addbuf(p);
 }
 if(msize)
 {
 recordpacket(1, ws.messages.getbuf(), msize);
 ucharbuf p = ws.messages.reserve(msize);
 p.put(ws.messages.getbuf(), msize);
 ws.messages.addbuf(p);
 }
 ws.uses = 0;
 if(psize || msize) loopv(clients)
 {
 clientinfo &ci = *clients[i];
 if(ci.state.aitype != AI_NONE) continue;
 ENetPacket *packet;
 if(psize && (ci.posoff<0 || psize-ci.poslen>0))
 {
 packet = enet_packet_create(&ws.positions[ci.posoff<0 ? 0 : ci.posoff+ci.poslen],
 ci.posoff<0 ? psize : psize-ci.poslen,
 ENET_PACKET_FLAG_NO_ALLOCATE);
 sendpacket(ci.clientnum, 0, packet);
 if(!packet->referenceCount) enet_packet_destroy(packet);
 else { ++ws.uses; packet->freeCallback = cleanworldstate; }
 }

 if(msize && (ci.msgoff<0 || msize-ci.msglen>0))
 {
 packet = enet_packet_create(&ws.messages[ci.msgoff<0 ? 0 : ci.msgoff+ci.msglen],
 ci.msgoff<0 ? msize : msize-ci.msglen,
 (reliablemessages ? ENET_PACKET_FLAG_RELIABLE : 0) | ENET_PACKET_FLAG_NO_ALLOCATE);
 sendpacket(ci.clientnum, 1, packet);
 if(!packet->referenceCount) enet_packet_destroy(packet);
 else { ++ws.uses; packet->freeCallback = cleanworldstate; }
 }
 }
 reliablemessages = false;
 if(!ws.uses)
 {
 delete &ws;
 return false;
 }
 else
 {
 worldstates.add(&ws);
 return true;
 }
 }

 bool sendpackets(bool force)
 {
 if(clients.empty() || (!hasnonlocalclients() && !demorecord)) return false;
 enet_uint32 curtime = enet_time_get()-lastsend;
 if(curtime<33 && !force) return false;
 bool flush = buildworldstate();
 lastsend += curtime - (curtime%33);
 return flush;
 }

 template<class T>
 void sendstate(gamestate &gs, T &p)
 {
 putint(p, gs.lifesequence);
 putint(p, gs.health);
 putint(p, gs.maxhealth);
 putint(p, gs.armour);
 putint(p, gs.armourtype);
 putint(p, gs.gunselect);
 loopi(GUN_PISTOL-GUN_SG+1) putint(p, gs.ammo[GUN_SG+i]);
 }

 void spawnstate(clientinfo *ci)
 {
 gamestate &gs = ci->state;
 gs.spawnstate(gamemode);
 gs.lifesequence = (gs.lifesequence + 1)&0x7F;
 }

 void sendspawn(clientinfo *ci)
 {
 gamestate &gs = ci->state;
 spawnstate(ci);
 sendf(ci->ownernum, 1, "rii7v", N_SPAWNSTATE, ci->clientnum, gs.lifesequence,
 gs.health, gs.maxhealth,
 gs.armour, gs.armourtype,
 gs.gunselect, GUN_PISTOL-GUN_SG+1, &gs.ammo[GUN_SG]);
 gs.lastspawn = gamemillis;
 }

 void sendwelcome(clientinfo *ci)
 {
 packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
 int chan = welcomepacket(p, ci);
 sendpacket(ci->clientnum, chan, p.finalize());
 }

 void putinitclient(clientinfo *ci, packetbuf &p)
 {
 if(ci->state.aitype != AI_NONE)
 {
 putint(p, N_INITAI);
 putint(p, ci->clientnum);
 putint(p, ci->ownernum);
 putint(p, ci->state.aitype);
 putint(p, ci->state.skill);
 putint(p, ci->playermodel);
 sendstring(ci->name, p);
 sendstring(ci->team, p);
 }
 else
 {
 putint(p, N_INITCLIENT);
 putint(p, ci->clientnum);
 sendstring(ci->name, p);
 sendstring(ci->team, p);
 putint(p, ci->playermodel);
 }
 }

 void welcomeinitclient(packetbuf &p, int exclude = -1)
 {
 loopv(clients)
 {
 clientinfo *ci = clients[i];
 if(!ci->connected || ci->clientnum == exclude) continue;

 putinitclient(ci, p);
 }
 }

 int welcomepacket(packetbuf &p, clientinfo *ci)
 {
 int hasmap = (m_edit && (clients.length()>1 || (ci && ci->local))) || (smapname[0] && (gamemillis<gamelimit || (ci && ci->state.state==CS_SPECTATOR && !ci->privilege && !ci->local) || numclients(ci ? ci->clientnum : -1, true, true, true)));
 putint(p, N_WELCOME);
 putint(p, hasmap);
 if(hasmap)
 {
 putint(p, N_MAPCHANGE);
 sendstring(smapname, p);
 putint(p, gamemode);
 putint(p, notgotitems ? 1 : 0);
 if(!ci || (m_timed && smapname[0]))
 {
 putint(p, N_TIMEUP);
 putint(p, gamemillis < gamelimit && !interm ? max((gamelimit - gamemillis)/1000, 1) : 0);
 }
 if(!notgotitems)
 {
 putint(p, N_ITEMLIST);
 loopv(sents) if(sents[i].spawned)
 {
 putint(p, i);
 putint(p, sents[i].type);
 }
 putint(p, -1);
 }
 }
 if(currentmaster >= 0 || mastermode != MM_OPEN)
 {
 putint(p, N_CURRENTMASTER);
 putint(p, currentmaster);
 clientinfo *m = currentmaster >= 0 ? getinfo(currentmaster) : NULL;
 putint(p, m ? m->privilege : 0);
 putint(p, mastermode);
 }
 if(gamepaused)
 {
 putint(p, N_PAUSEGAME);
 putint(p, 1);
 }
 if(ci)
 {
 putint(p, N_SETTEAM);
 putint(p, ci->clientnum);
 sendstring(ci->team, p);
 putint(p, -1);
 }
 if(ci && (m_demo || m_mp(gamemode)) && ci->state.state!=CS_SPECTATOR)
 {
 if(smode && !smode->canspawn(ci, true))
 {
 ci->state.state = CS_DEAD;
 putint(p, N_FORCEDEATH);
 putint(p, ci->clientnum);
 sendf(-1, 1, "ri2x", N_FORCEDEATH, ci->clientnum, ci->clientnum);
 }
 else
 {
 gamestate &gs = ci->state;
 spawnstate(ci);
 putint(p, N_SPAWNSTATE);
 putint(p, ci->clientnum);
 sendstate(gs, p);
 gs.lastspawn = gamemillis;
 }
 }
 if(ci && ci->state.state==CS_SPECTATOR)
 {
 putint(p, N_SPECTATOR);
 putint(p, ci->clientnum);
 putint(p, 1);
 sendf(-1, 1, "ri3x", N_SPECTATOR, ci->clientnum, 1, ci->clientnum);
 }
 if(!ci || clients.length()>1)
 {
 putint(p, N_RESUME);
 loopv(clients)
 {
 clientinfo *oi = clients[i];
 if(ci && oi->clientnum==ci->clientnum) continue;
 putint(p, oi->clientnum);
 putint(p, oi->state.state);
 putint(p, oi->state.frags);
 putint(p, oi->state.flags);
 putint(p, oi->state.quadmillis);
 sendstate(oi->state, p);
 }
 putint(p, -1);
 welcomeinitclient(p, ci ? ci->clientnum : -1);
 }
 if(smode) smode->initclient(ci, p, true);
 return 1;
 }

 bool restorescore(clientinfo *ci)
 {
 //if(ci->local) return false;
 savedscore &sc = findscore(ci, false);
 if(&sc)
 {
 sc.restore(ci->state);
 return true;
 }
 return false;
 }

 void sendresume(clientinfo *ci)
 {
 gamestate &gs = ci->state;
 sendf(-1, 1, "ri3i9vi", N_RESUME, ci->clientnum,
 gs.state, gs.frags, gs.flags, gs.quadmillis,
 gs.lifesequence,
 gs.health, gs.maxhealth,
 gs.armour, gs.armourtype,
 gs.gunselect, GUN_PISTOL-GUN_SG+1, &gs.ammo[GUN_SG], -1);
 }

 void sendinitclient(clientinfo *ci)
 {
 packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
 putinitclient(ci, p);
 sendpacket(-1, 1, p.finalize(), ci->clientnum);
 }
 int servuptime = 0;
 void changemap(const char *s, int mode)
 {
 out(ECHO_SERV, "Loaded map: \f1%s", s);
 out(ECHO_CONSOLE, "Loaded map: %s", s);
 out(ECHO_IRC, "Loaded map: \x02%s\x02", s);
 if(persist && !m_edit) out(ECHO_SERV, "Persistant teams currently enabled");
 stopdemo();
 pausegame(false);
 if(smode) smode->reset(false);
 aiman::clearai();
 mapreload = false;
 gamemode = mode;
 servuptime=(gamemillis/1000)+servuptime;
 gamemillis = 0;
 gamelimit = (m_overtime ? 15 : 10)*60000;
 interm = 0;
 nextexceeded = 0;
 copystring(smapname, s);
 resetitems();
 notgotitems = true;
 scores.shrink(0);
 loopv(clients)
 {
 clientinfo *ci = clients[i];
 ci->state.timeplayed += lastmillis - ci->state.lasttimeplayed;
 }

 if(!m_mp(gamemode)) kicknonlocalclients(DISC_PRIVATE);

 if(m_teammode && !persist) autoteam();

 if(m_capture) smode = &capturemode;
 else if(m_ctf) smode = &ctfmode;
 else smode = NULL;
 if(smode) smode->reset(false);

 if(m_timed && smapname[0]) sendf(-1, 1, "ri2", N_TIMEUP, gamemillis < gamelimit && !interm ? max((gamelimit - gamemillis)/1000, 1) : 0);
 loopv(clients)
 {
 clientinfo *ci = clients[i];
 ci->mapchange();
 ci->state.lasttimeplayed = lastmillis;
 if(m_mp(gamemode) && ci->state.state!=CS_SPECTATOR) sendspawn(ci);
 }

 aiman::changemap();

 if(m_demo)
 {
 if(clients.length()) setupdemoplayback();
 }
 else if(demonextmatch)
 {
 demonextmatch = false;
 setupdemorecord();
 }

firstblood = false;
 }

 struct votecount
 {
 char *map;
 int mode, count;
 votecount() {}
 votecount(char *s, int n) : map(s), mode(n), count(0) {}
 };

 void checkvotes(bool force = false)
 {
 vector<votecount> votes;
 int maxvotes = 0;
 loopv(clients)
 {
 clientinfo *oi = clients[i];
 if(oi->state.state==CS_SPECTATOR && !oi->privilege && !oi->local) continue;
 if(oi->state.aitype!=AI_NONE) continue;
 maxvotes++;
 if(!oi->mapvote[0]) continue;
 votecount *vc = NULL;
 loopvj(votes) if(!strcmp(oi->mapvote, votes[j].map) && oi->modevote==votes[j].mode)
 {
 vc = &votes[j];
 break;
 }
 if(!vc) vc = &votes.add(votecount(oi->mapvote, oi->modevote));
 vc->count++;
 }
 votecount *best = NULL;
 loopv(votes) if(!best || votes[i].count > best->count || (votes[i].count == best->count && rnd(2))) best = &votes[i];
 if(force || (best && best->count > maxvotes/2))
 {
 if(demorecord) enddemorecord();
 if(best && (best->count > (force ? 1 : maxvotes/2)))
 {
 sendservmsg(force ? "Vote passed by \f4Default" : "Vote passed by \f0Majority");
 sendf(-1, 1, "risii", N_MAPCHANGE, best->map, best->mode, 1);
 changemap(best->map, best->mode);
 }
 else
 {
 mapreload = true;
 if(clients.length()) sendf(-1, 1, "ri", N_MAPRELOAD);
 }
 }
 }

 void forcemap(const char *map, int mode)
 {
 stopdemo();
 if(hasnonlocalclients() && !mapreload)
 {
 defformatstring(msg)("\f6Host \f7forced \f2%s \f7on map \f1%s", modename(mode), map);
 sendservmsg(msg);
 }
 sendf(-1, 1, "risii", N_MAPCHANGE, map, mode, 1);
 changemap(map, mode);
 }

 void vote(char *map, int reqmode, int sender)
 {
 clientinfo *ci = getinfo(sender);
 if(!ci || (ci->state.state==CS_SPECTATOR && !ci->privilege && !ci->local) || (!ci->local && !m_mp(reqmode))) return;
 copystring(ci->mapvote, map);
 ci->modevote = reqmode;
 if(!ci->mapvote[0]) return;
 if(ci->local || mapreload || (ci->privilege && mastermode>=MM_VETO))
 {
 if(demorecord) enddemorecord();
 if((!ci->local || hasnonlocalclients()) && !mapreload)
 {
 defformatstring(msg)("%s \f7forced \f2%s \f7on map \f1%s", ci->privilege && mastermode>=MM_VETO ? privname(ci->privilege) : "\f6Host", modename(ci->modevote), ci->mapvote);
 sendservmsg(msg);
 }
 sendf(-1, 1, "risii", N_MAPCHANGE, ci->mapvote, ci->modevote, 1);
 changemap(ci->mapvote, ci->modevote);
 }
 else
 {
 defformatstring(msg)("\f0%s \f7votes for a \f2%s \f7on map \f1%s \f7(use \f2\"/mode map\" \f7to vote)", colorname(ci),modename(reqmode), map, map);
 sendservmsg(msg);
 out(ECHO_IRC, "%s votes for a %s on map %s", colorname(ci), modename(reqmode), map);
 checkvotes();
 }
 }

 void checkintermission()
 {
 if(gamemillis >= gamelimit && !interm)
 {
 sendf(-1, 1, "ri2", N_TIMEUP, 0);
 if(smode) smode->intermission();
 interm = gamemillis + 10000;
 }
 }

 void startintermission() { gamelimit = min(gamelimit, gamemillis); checkintermission(); }

 void suicide(clientinfo *ci)
 {
 gamestate &gs = ci->state;
 if(gs.state!=CS_ALIVE) return;
 ci->state.frags += smode ? smode->fragvalue(ci, ci) : -1;
 ci->state.deaths++;
 sendf(-1, 1, "ri4", N_DIED, ci->clientnum, ci->clientnum, gs.frags);
 ci->position.setsize(0);
 if(smode) smode->died(ci, NULL);
 gs.state = CS_DEAD;
 gs.respawn();
 }

 void suicideevent::process(clientinfo *ci)
 {
 suicide(ci);
 }

struct spreemsg {
int frags;
string msg1, msg2;
};

vector <spreemsg> spreemessages;
ICOMMAND(addspreemsg, "iss", (int *frags, char *msg1, char *msg2), { spreemsg m; m.frags = *frags; copystring(m.msg1, msg1); copystring(m.msg2, msg2); spreemessages.add(m); });
struct multikillmsg {
int frags;
string msg;
};
vector <multikillmsg> multikillmessages;
SVAR(defmultikillmsg, "MULTI KILL"); //default message for multikills. The message is followed by the number of frags 
VAR(minmultikill, 2, 2, INT_MAX); // minimum number of kills for a multi-kill to occur 
ICOMMAND(addmultikillmsg, "is", (int *frags, char *msg), { multikillmsg m; m.frags = *frags; copystring(m.msg, msg); multikillmessages.add(m); });

 void dodamage(clientinfo *target, clientinfo *actor, int damage, int gun, const vec &hitpush = vec(0, 0, 0))
 {
 gamestate &ts = target->state;
 ts.dodamage(damage);
 actor->state.damage += damage;
 sendf(-1, 1, "ri6", N_DAMAGE, target->clientnum, actor->clientnum, damage, ts.armour, ts.health);
 if(target==actor) target->setpushed();
 else if(target!=actor && !hitpush.iszero())
 {
 ivec v = vec(hitpush).rescale(DNF);
 sendf(ts.health<=0 ? -1 : target->ownernum, 1, "ri7", N_HITPUSH, target->clientnum, gun, damage, v.x, v.y, v.z);
 target->setpushed();
 }
 if(ts.health<=0)
 {
 target->state.deaths++;
 if(isteam(actor->team, target->team))
{
 actor->state.teamkills++;
 target->state.deaths++;
if(actor == target) {} //suicide is not a teamkill 
else {
 defformatstring(msg)("\f0%s \f7fragged his teammate \f6%s\f7", colorname(actor), colorname(target));
sendservmsg(msg);
if(getvar("tkpenalty")) { 
if(actor->state.state==CS_ALIVE) {
suicide(actor); 
defformatstring(teamkillmsg)("\f1Notice: \f7%s", tkmsg); //Teamkill message
sendf(actor->clientnum, 1, "ris", N_SERVMSG, teamkillmsg);}
 }
} 
}

 if(actor!=target && isteam(actor->team, target->team)) actor->state.teamkills++;
 int fragvalue = smode ? smode->fragvalue(target, actor) : (target==actor || isteam(target->team, actor->team) ? -1 : 1);
 actor->state.frags += fragvalue;
 if(fragvalue>0)
 {
 int friends = 0, enemies = 0; //friends also includes the fragger
 if(m_teammode) loopv(clients) if(strcmp(clients[i]->team, actor->team)) enemies++; else friends++;
 else { friends = 1; enemies = clients.length()-1; }
 actor->state.effectiveness += fragvalue*friends/float(max(enemies, 1));
 }
 sendf(-1, 1, "ri4", N_DIED, target->clientnum, actor->clientnum, actor->state.frags);
 if(!firstblood && actor != target) { firstblood = true; out(ECHO_SERV, "\f0%s \f7commited \f6THE FIRST SLAUGHTER!", colorname(actor)); }
if(actor != target) actor->state.spreefrags++;
if(target->state.spreefrags >= minspreefrags) {
if(actor == target)
out(ECHO_SERV, "\f0%s \f7%s", colorname(target), spreesuicidemsg);
else
out(ECHO_SERV, "\f0%s's \f7%s \f6%s", colorname(target), spreefinmsg, colorname(actor));
}
target->state.spreefrags = 0;
target->state.multifrags = 0;
target->state.lastfragmillis = 0;
loopv(spreemessages) {
if(actor->state.spreefrags == spreemessages[i].frags) out(ECHO_SERV, "\f0%s \f7%s \f6%s", colorname(actor), spreemessages[i].msg1, spreemessages[i].msg2);
}
 target->position.setsize(0);
 if(smode) smode->died(target, actor);
 ts.state = CS_DEAD;
 ts.lastdeath = gamemillis;
 }
}
 void explodeevent::process(clientinfo *ci)
 {
 gamestate &gs = ci->state;
 switch(gun)
 {
 case GUN_RL:
 if(!gs.rockets.remove(id)) return;
 break;

 case GUN_GL:
 if(!gs.grenades.remove(id)) return;
 break;

 default:
 return;
 }
 sendf(-1, 1, "ri4x", N_EXPLODEFX, ci->clientnum, gun, id, ci->ownernum);
 loopv(hits)
 {
 hitinfo &h = hits[i];
 clientinfo *target = getinfo(h.target);
 if(!target || target->state.state!=CS_ALIVE || h.lifesequence!=target->state.lifesequence || h.dist<0 || h.dist>RL_DAMRAD) continue;

 bool dup = false;
 loopj(i) if(hits[j].target==h.target) { dup = true; break; }
 if(dup) continue;

 int damage = guns[gun].damage;
 if(gs.quadmillis) damage *= 4;
 damage = int(damage*(1-h.dist/RL_DISTSCALE/RL_DAMRAD));
 if(gun==GUN_RL && target==ci) damage /= RL_SELFDAMDIV;
 dodamage(target, ci, damage, gun, h.dir);
 }
 }

 void shotevent::process(clientinfo *ci)
 {
 gamestate &gs = ci->state;
 int wait = millis - gs.lastshot;
 if(!gs.isalive(gamemillis) ||
 wait<gs.gunwait ||
 gun<GUN_FIST || gun>GUN_PISTOL ||
 gs.ammo[gun]<=0 || (guns[gun].range && from.dist(to) > guns[gun].range + 1))
 return;
 if(gun!=GUN_FIST) gs.ammo[gun]--;
 gs.lastshot = millis;
 gs.gunwait = guns[gun].attackdelay;
 sendf(-1, 1, "rii9x", N_SHOTFX, ci->clientnum, gun, id,
 int(from.x*DMF), int(from.y*DMF), int(from.z*DMF),
 int(to.x*DMF), int(to.y*DMF), int(to.z*DMF),
 ci->ownernum);
 gs.shotdamage += guns[gun].damage*(gs.quadmillis ? 4 : 1)*(gun==GUN_SG ? SGRAYS : 1);
 switch(gun)
 {
 case GUN_RL: gs.rockets.add(id); break;
 case GUN_GL: gs.grenades.add(id); break;
 default:
 {
 int totalrays = 0, maxrays = gun==GUN_SG ? SGRAYS : 1;
 loopv(hits)
 {
 hitinfo &h = hits[i];
 clientinfo *target = getinfo(h.target);
 if(!target || target->state.state!=CS_ALIVE || h.lifesequence!=target->state.lifesequence || h.rays<1 || h.dist > guns[gun].range + 1) continue;

 totalrays += h.rays;
 if(totalrays>maxrays) continue;
 int damage = h.rays*guns[gun].damage;
 if(gs.quadmillis) damage *= 4;
 dodamage(target, ci, damage, gun, h.dir);
 }
 break;
 }
 }
 }

 void pickupevent::process(clientinfo *ci)
 {
 gamestate &gs = ci->state;
 if(m_mp(gamemode) && !gs.isalive(gamemillis)) return;
 pickup(ent, ci->clientnum);
 }

 bool gameevent::flush(clientinfo *ci, int fmillis)
 {
 process(ci);
 return true;
 }
 void clearevent(clientinfo *ci)
 {
 delete ci->events.remove(0);
 }


 bool timedevent::flush(clientinfo *ci, int fmillis)
 {
 if(millis > fmillis) return false;
 else if(millis >= ci->lastevent)
 {
 ci->lastevent = millis;
 process(ci);
 }
 return true;
 }

 void flushevents(clientinfo *ci, int millis)
 {
 while(ci->events.length())
 {
 gameevent *ev = ci->events[0];
 if(ev->flush(ci, millis)) clearevent(ci);
 else break;
 }
 }

 void processevents()
 {
 loopv(clients)
 {
 clientinfo *ci = clients[i];
 if(curtime>0 && ci->state.quadmillis) ci->state.quadmillis = max(ci->state.quadmillis-curtime, 0);
 flushevents(ci, gamemillis);
 }
 }
 vector<char *> banners;
 ICOMMAND(addbanner, "s", (char *text), {
 banners.add(newstring(text));
 });

 VAR(bannerintervolmillis, 1000, 10000, 500000);
 void updateBanner()
 {
 static int lastshow = lastmillis;
 if(banners.length() > 0)
 {
 if((lastmillis-lastshow) >= bannerintervolmillis)
 {
 defformatstring(text)("%s", banners[rnd(banners.length())]);
 sendservmsg(text);
 lastshow = lastmillis;
 } else return;
 }
 }

 void cleartimedevents(clientinfo *ci)
 {
 int keep = 0;
 loopv(ci->events)
 {
 if(ci->events[i]->keepable())
 {
 if(keep < i)
 {
 for(int j = keep; j < i; j++) delete ci->events[j];
 ci->events.remove(keep, i - keep);
 i = keep;
 }
 keep = i+1;
 continue;
 }
 }
 while(ci->events.length() > keep) delete ci->events.pop();
 ci->timesync = false;
 }

 bool ispaused() { return gamepaused; }

 void serverupdate()
 {
 if(!gamepaused) gamemillis += curtime;

 if(m_demo) readdemo();
 else if(!gamepaused && (!m_timed || gamemillis < gamelimit))
 {
 processevents();
 if(curtime)
 {
 updateBanner();
 loopv(sents) if(sents[i].spawntime) //spawn entities when timer reached
 {
 int oldtime = sents[i].spawntime;
 sents[i].spawntime -= curtime;
 if(sents[i].spawntime<=0)
 {
 sents[i].spawntime = 0;
 sents[i].spawned = true;
 sendf(-1, 1, "ri2", N_ITEMSPAWN, i);
 }
 else if(sents[i].spawntime<=10000 && oldtime>10000 && (sents[i].type==I_QUAD || sents[i].type==I_BOOST))
 {
 sendf(-1, 1, "ri2", N_ANNOUNCE, sents[i].type);
 }
 }
 }
 aiman::checkai();
 if(smode) smode->update();
 }

 while(bannedips.length() && bannedips[0].time-totalmillis>4*60*60000) bannedips.remove(0);
 loopv(connects) if(totalmillis-connects[i]->connectmillis>15000) disconnect_client(connects[i]->clientnum, DISC_TIMEOUT);

 if(nextexceeded && gamemillis > nextexceeded && (!m_timed || gamemillis < gamelimit))
 {
 nextexceeded = 0;
 loopvrev(clients)
 {
 clientinfo &c = *clients[i];
 if(c.state.aitype != AI_NONE) continue;
 if(c.checkexceeded()) disconnect_client(c.clientnum, DISC_TAGT);
 else c.scheduleexceeded();
 }
 }

 if(!gamepaused && m_timed && smapname[0] && gamemillis-curtime>0) checkintermission();
 if(interm > 0 && gamemillis>interm)
 {
 if(demorecord) enddemorecord();
 interm = -1;
 checkvotes(true);
 }
 }

 struct crcinfo
 {
 int crc, matches;
 crcinfo() {}
 crcinfo(int crc, int matches) : crc(crc), matches(matches) {}

 static int compare(const crcinfo *x, const crcinfo *y)
 {
 if(x->matches > y->matches) return -1;
 if(x->matches < y->matches) return 1;
 return 0;
 }
 };

 void checkmaps(int req = -1)
 {
 if(m_edit || !smapname[0]) return;
 vector<crcinfo> crcs;
 int total = 0, unsent = 0, invalid = 0;
 loopv(clients)
 {
 clientinfo *ci = clients[i];
 if(ci->state.state==CS_SPECTATOR || ci->state.aitype != AI_NONE) continue;
 total++;
 if(!ci->clientmap[0])
 {
 if(ci->mapcrc < 0) invalid++;
 else if(!ci->mapcrc) unsent++;
 }
 else
 {
 crcinfo *match = NULL;
 loopvj(crcs) if(crcs[j].crc == ci->mapcrc) { match = &crcs[j]; break; }
 if(!match) crcs.add(crcinfo(ci->mapcrc, 1));
 else match->matches++;
 }
 }
 if(total - unsent < min(total, 4)) return;
 crcs.sort(crcinfo::compare);
 string msg;
 //Modified Map 
 loopv(clients)
 {
 clientinfo *ci = clients[i];
 if(ci->state.state==CS_SPECTATOR || ci->state.aitype != AI_NONE || ci->clientmap[0] || ci->mapcrc >= 0 || (req < 0 && ci->warned)) continue;
 formatstring(msg)("\f3Warning: \f0%s \f7has modified map: \f1\"%s\"", colorname(ci), smapname);
 sendf(req, 1, "ris", N_SERVMSG, msg);
 if(req < 0) ci->warned = true;
 }
 if(crcs.empty() || crcs.length() < 2) return;
 loopv(crcs)
 {
 crcinfo &info = crcs[i];
 if(i || info.matches <= crcs[i+1].matches) loopvj(clients)
 {
 clientinfo *ci = clients[j];
 if(ci->state.state==CS_SPECTATOR || ci->state.aitype != AI_NONE || !ci->clientmap[0] || ci->mapcrc != info.crc || (req < 0 && ci->warned)) continue;
 formatstring(msg)("\f3Warning: \f0%s \f7has modified map: \f1\"%s\"", colorname(ci), smapname);
 sendf(req, 1, "ris", N_SERVMSG, msg);
 if(req < 0) ci->warned = true;
 }
 }
 }

 void sendservinfo(clientinfo *ci)
 {
 sendf(ci->clientnum, 1, "ri5s", N_SERVINFO, ci->clientnum, PROTOCOL_VERSION, ci->sessionid, serverpass[0] ? 1 : 0, serverdesc);
 }

 //Empty Server
 void noclients()
 {
 aiman::clearai();
 printf("\33[33mServer has emptied\33[0m\n");
 //bannedips.shrink(0); Don't delete banned IP's
 }

 void localconnect(int n)
 {
 clientinfo *ci = getinfo(n);
 ci->clientnum = ci->ownernum = n;
 ci->connectmillis = totalmillis;
 ci->sessionid = (rnd(0x1000000)*((totalmillis%10000)+1))&0xFFFFFF;
 ci->local = true;

 connects.add(ci);
 sendservinfo(ci);
 }

 void localdisconnect(int n)
 {
 if(m_demo) enddemoplayback();
 clientdisconnect(n);
 }

 int clientconnect(int n, uint ip, char *ipstr)
 {
 clientinfo *ci = getinfo(n);
 ci->ip=ipstr; //ipstring
 ci->clientnum = ci->ownernum = n;
 ci->connectmillis = totalmillis;
 ci->sessionid = (rnd(0x1000000)*((totalmillis%10000)+1))&0xFFFFFF;

 connects.add(ci);
 if(!m_mp(gamemode)) return DISC_PRIVATE;
 sendservinfo(ci);
 return DISC_NONE;
 }


 void clientdisconnect(int n)
 {
 clientinfo *ci = getinfo(n);

 if(ci->connected)
 {
clients.removeobj(ci);
aiman::removeai(ci);
if(smode) smode->leavegame(ci, true);
 if(ci->privilege) setmaster(ci, false);
 ci->state.timeplayed+=lastmillis-ci->state.lasttimeplayed;
out(ECHO_CONSOLE, "%s (%s) disconnected", ci->name, ci->ip); 
irc.speak("%s (%s) disconnected", colorname(ci), ci->ip);
savescore(ci);
 sendf(-1, 1, "ri2", N_CDIS, n);
 if(!numclients(-1, false, true)) noclients(); 
 }
 else connects.removeobj(ci);
 }

 int reserveclients() { return 3; }

 struct gbaninfo
 {
 enet_uint32 ip, mask;
 };

 vector<gbaninfo> gbans;

 void cleargbans()
 {
 gbans.shrink(0);
 }

 bool checkgban(uint ip)
 {
 loopv(gbans) if((ip & gbans[i].mask) == gbans[i].ip) return true;
 return false;
 }

 void addgban(const char *name)
 {
 union { uchar b[sizeof(enet_uint32)]; enet_uint32 i; } ip, mask;
 ip.i = 0;
 mask.i = 0;
 loopi(4)
 {
 char *end = NULL;
 int n = strtol(name, &end, 10);
 if(!end) break;
 if(end > name) { ip.b[i] = n; mask.b[i] = 0xFF; }
 name = end;
 while(*name && *name++ != '.');
 }
 gbaninfo &ban = gbans.add();
 ban.ip = ip.i;
 ban.mask = mask.i;

 loopvrev(clients)
 {
 clientinfo *ci = clients[i];
 if(ci->local || ci->privilege >= PRIV_ADMIN) continue;
 if(checkgban(getclientip(ci->clientnum))) disconnect_client(ci->clientnum, DISC_IPBAN);
 }
 }
 
 int allowconnect(clientinfo *ci, const char *pwd)
 {
 if(ci->local) return DISC_NONE;
 if(!m_mp(gamemode)) return DISC_PRIVATE;
 if(serverpass[0])
 {
 if(!checkpassword(ci, serverpass, pwd)) return DISC_PRIVATE;
 return DISC_NONE;
 }
 if(adminpass[0] && checkpassword(ci, adminpass, pwd)) return DISC_NONE;
 if(numclients(-1, false, true)>=maxclients) return DISC_MAXCLIENTS;
 uint ip = getclientip(ci->clientnum);
 loopv(bannedips) if(bannedips[i].ip==ip) return DISC_IPBAN;
 if(checkgban(ip)) return DISC_IPBAN;
 if(mastermode>=MM_PRIVATE && allowedips.find(ip)<0) return DISC_PRIVATE;
 return DISC_NONE;
 }

 bool allowbroadcast(int n)
 {
 clientinfo *ci = getinfo(n);
 return ci && ci->connected;
 }

 clientinfo *findauth(uint id)
 {
 loopv(clients) if(clients[i]->authreq == id) return clients[i];
 return NULL;
 }

 void authfailed(uint id)
 {
 clientinfo *ci = findauth(id);
 if(!ci) return;
 ci->authreq = 0;
 }

 void authsucceeded(uint id)
 {
 clientinfo *ci = findauth(id);
 if(!ci) return;
 ci->authreq = 0;
 setmaster(ci, true, "", ci->authname);
 }

 void authchallenged(uint id, const char *val)
 {
 clientinfo *ci = findauth(id);
 if(!ci) return;
 sendf(ci->clientnum, 1, "risis", N_AUTHCHAL, "", id, val);
 }

 uint nextauthreq = 0;

 void tryauth(clientinfo *ci, const char *user)
 {
 if(!nextauthreq) nextauthreq = 1;
 ci->authreq = nextauthreq++;
 filtertext(ci->authname, user, false, 100);
 if(!requestmasterf("reqauth %u %s\n", ci->authreq, ci->authname))
 {
 ci->authreq = 0;
 sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f3Error: \f7Not connected to authentication server");
 }
 }

 void answerchallenge(clientinfo *ci, uint id, char *val)
 {
 if(ci->authreq != id) return;
 for(char *s = val; *s; s++)
 {
 if(!isxdigit(*s)) { *s = '\0'; break; }
 }
 if(!requestmasterf("confauth %u %s\n", id, val))
 {
 ci->authreq = 0;
 sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f3Error: \f7Not connected to authentication server");
 }
 }

 void processmasterinput(const char *cmd, int cmdlen, const char *args)
 {
 uint id;
 string val;
 if(sscanf(cmd, "failauth %u", &id) == 1)
 authfailed(id);
 else if(sscanf(cmd, "succauth %u", &id) == 1)
 authsucceeded(id);
 else if(sscanf(cmd, "chalauth %u %s", &id, val) == 2)
 authchallenged(id, val);
 else if(!strncmp(cmd, "cleargbans", cmdlen))
 cleargbans();
 else if(sscanf(cmd, "addgban %s", val) == 1)
 addgban(val);
 }

 void receivefile(int sender, uchar *data, int len)
 {
 if(!m_edit || len > 1024*1024) return;
 clientinfo *ci = getinfo(sender);
 if(ci->state.state==CS_SPECTATOR && !ci->privilege && !ci->local) return;
 if(mapdata) DELETEP(mapdata);
 if(!len) return;
 mapdata = opentempfile("mapdata", "w+b");
 if(!mapdata) { sendf(sender, 1, "ris", N_SERVMSG, "\f3Error: \f7Failed to open temporary file for map"); return; }
 mapdata->write(data, len);
 defformatstring(msg)("\f0%s \f7uploaded a map to the server, type \f2\"/getmap\" \f7to download it", colorname(ci));
 sendservmsg(msg);
out(ECHO_CONSOLE, "%s uploaded map \"%s\" to the server", colorname(ci), smapname);
 }

 void sendclipboard(clientinfo *ci)
 {
 if(!ci->lastclipboard || !ci->clipboard) return;
 bool flushed = false;
 loopv(clients)
 {
 clientinfo &e = *clients[i];
 if(e.clientnum != ci->clientnum && e.needclipboard - ci->lastclipboard >= 0)
 {
 if(!flushed) { flushserver(true); flushed = true; }
 sendpacket(e.clientnum, 1, ci->clipboard);
 }
 }
 }

 char *invisadmin()
 {
 defformatstring(text)("invadmin %s", adminpass);
 char *outtext = text;
 return outtext;
 }

 void parsepacket(int sender, int chan, packetbuf &p) //Parses exactly each byte of the packet
 {
 if(sender<0) return;
 char text[MAXTRANS];
 int type;
 clientinfo *ci = sender>=0 ? getinfo(sender) : NULL, *cq = ci, *cm = ci;
 if(ci && !ci->connected)
 {
 if(chan==0) return;
 else if(chan!=1 || getint(p)!=N_CONNECT) {disconnect_client(sender, DISC_TAGT); return;}
 else
 {
 getstring(text, p);
 filtertext(text, text, false, MAXNAMELEN);
 if(!text[0]) copystring(text, "unnamed");
 copystring(ci->name, text, MAXNAMELEN+1);
 getstring(text, p);
 int disc = allowconnect(ci, text);
 if(disc)
 {
 disconnect_client(sender, disc);
 return;
 }
     //Client connect
     clients.add(ci);
     ci->connected = true;
     connects.removeobj(ci);
     ci->playermodel = getint(p);
     if(mastermode>=MM_LOCKED) ci->state.state = CS_SPECTATOR;
     if(getvar("enablegeoip")) {
         GeoIP *gi;
         gi = GeoIP_open("./GeoIP.dat",GEOIP_STANDARD);
         defformatstring(ip)("%s", GeoIP_country_name_by_name(gi, ci->ip));
         if(!strcmp("(null)", ip)){ 
             out(ECHO_SERV, "\f0%s \f7connected from an \f4Unknown Location", colorname(ci), ci->ip); 
             out(ECHO_MASTER, "\f0%s \f7(\f4%s\f7)", colorname(ci), ci->ip); //only tell master IP of client
             irc.speak("%s (%s) connected from an Unknown Location", colorname(ci), ci->ip);
         }else{
             out(ECHO_SERV, "\f0%s \f7connected from \f2%s", colorname(ci), ip);
             irc.speak("%s (%s) connected from %s", colorname(ci), ci->ip, ip);
         }
     }
     else if(!getvar("enablegeoip")) { 
         out(ECHO_SERV, "\f0%s \f7connected", colorname(ci));
         out(ECHO_CONSOLE, "%s connected", colorname(ci));
         irc.speak("%s (%s) connected", colorname(ci), ci->ip);
     }
     if(m_demo) enddemoplayback();
     ci->state.lasttimeplayed = lastmillis;
     ci->needclipboard = totalmillis;
     const char *worst = m_teammode ? chooseworstteam(NULL, ci) : NULL;
     copystring(ci->team, worst ? worst : "good", MAXTEAMLEN+1);
     if(restorescore(ci)) sendresume(ci);
     aiman::addclient(ci);
     sendinitclient(ci);
     sendwelcome(ci);
     luaCallback(LUAEVENT_CONNECTED, ci->clientnum);
     out(ECHO_CONSOLE, "%s (%s) connected", ci->name, ci->ip);
     char *servername = serverdesc; 
     defformatstring(l)("\f4*****************************************\n\f4Welcome to %s\f4, \f0%s\f4! \n%s \n\f4***************************************** \n\f4Use \f6\"#cmds\" \f4to see all available commands.", servername, colorname(ci), welcomemsg);
     sendf(sender, 1, "ris", N_SERVMSG, l);
     if(m_demo) setupdemoplayback();
 }
 }
 else if(chan==2)
 {
 receivefile(sender, p.buf, p.maxlen);
 return;
 }
 if(p.packet->flags&ENET_PACKET_FLAG_RELIABLE) reliablemessages = true;
 #define QUEUE_AI clientinfo *cm = cq;
 #define QUEUE_MSG { if(cm && (!cm->local || demorecord || hasnonlocalclients())) while(curmsg<p.length()) cm->messages.add(p.buf[curmsg++]); }
 #define QUEUE_BUF(body) { \
 if(cm && (!cm->local || demorecord || hasnonlocalclients())) \
 { \
 curmsg = p.length(); \
 { body; } \
 } \
 }
 #define QUEUE_INT(n) QUEUE_BUF(putint(cm->messages, n))
 #define QUEUE_UINT(n) QUEUE_BUF(putuint(cm->messages, n))
 #define QUEUE_STR(text) QUEUE_BUF(sendstring(text, cm->messages))
 int curmsg;
 while((curmsg = p.length()) < p.maxlen) switch(type = checktype(getint(p), ci))
 {
 case N_POS:
 {
 int pcn = getuint(p);
 p.get();
 uint flags = getuint(p);
 clientinfo *cp = getinfo(pcn);
 if(cp && pcn != sender && cp->ownernum != sender) cp = NULL;
 vec pos;
 loopk(3)
 {
 int n = p.get(); n |= p.get()<<8; if(flags&(1<<k)) { n |= p.get()<<16; if(n&0x800000) n |= -1<<24; }
 pos[k] = n/DMF;
 }
 loopk(3) p.get();
 int mag = p.get(); if(flags&(1<<3)) mag |= p.get()<<8;
 int dir = p.get(); dir |= p.get()<<8;
 vec vel = vec((dir%360)*RAD, (clamp(dir/360, 0, 180)-90)*RAD).mul(mag/DVELF);
 if(flags&(1<<4))
 {
 p.get(); if(flags&(1<<5)) p.get();
 if(flags&(1<<6)) loopk(2) p.get();
 }
 if(cp)
 {
 if((!ci->local || demorecord || hasnonlocalclients()) && (cp->state.state==CS_ALIVE || cp->state.state==CS_EDITING))
 {
 if(!ci->local && !m_edit && max(vel.magnitude2(), (float)fabs(vel.z)) >= 180)
 cp->setexceeded();
 cp->position.setsize(0);
 while(curmsg<p.length()) cp->position.add(p.buf[curmsg++]);
 }
 if(smode && cp->state.state==CS_ALIVE) smode->moved(cp, cp->state.o, cp->gameclip, pos, (flags&0x80)!=0);
 cp->state.o = pos;
 cp->gameclip = (flags&0x80)!=0;
 }
 break;
 }

 case N_TELEPORT:
 {
 int pcn = getint(p), teleport = getint(p), teledest = getint(p);
 clientinfo *cp = getinfo(pcn);
 if(cp && pcn != sender && cp->ownernum != sender) cp = NULL;
 if(cp && (!ci->local || demorecord || hasnonlocalclients()) && (cp->state.state==CS_ALIVE || cp->state.state==CS_EDITING))
 {
 flushclientposition(*cp);
 sendf(-1, 0, "ri4x", N_TELEPORT, pcn, teleport, teledest, cp->ownernum);
 if(getvar("conteleport")) {out(ECHO_CONSOLE, "%s teleported", cp->name);}
 }
 break;
 }

 case N_JUMPPAD:
 {
 int pcn = getint(p), jumppad = getint(p);
 clientinfo *cp = getinfo(pcn);
 if(cp && pcn != sender && cp->ownernum != sender) cp = NULL;
 if(cp && (!ci->local || demorecord || hasnonlocalclients()) && (cp->state.state==CS_ALIVE || cp->state.state==CS_EDITING))
 {
 cp->setpushed();
 flushclientposition(*cp);
 sendf(-1, 0, "ri3x", N_JUMPPAD, pcn, jumppad, cp->ownernum);
 }
 break;
 }

 case N_FROMAI:
 {
 int qcn = getint(p);
 if(qcn < 0) cq = ci;
 else
 {
 cq = getinfo(qcn);
 if(cq && qcn != sender && cq->ownernum != sender) cq = NULL;
 }
 break;
 }

 case N_EDITMODE:
 {
 int val = getint(p);
 if(!ci->local && !m_edit) break;
 if(val ? ci->state.state!=CS_ALIVE && ci->state.state!=CS_DEAD : ci->state.state!=CS_EDITING) break;
 if(smode)
 {
 if(val) smode->leavegame(ci);
 else smode->entergame(ci);
 }
 if(val)
 {
 ci->state.editstate = ci->state.state;
 ci->state.state = CS_EDITING;
 ci->events.setsize(0);
 ci->state.rockets.reset();
 ci->state.grenades.reset();
 out(ECHO_CONSOLE, "%s entered edit mode", colorname(ci));
 }
 else ci->state.state = ci->state.editstate;
 QUEUE_MSG;
 break;
 }

 case N_MAPCRC:
 {
 getstring(text, p);
 int crc = getint(p);
 if(!ci) break;
 if(strcmp(text, smapname))
 {
 if(ci->clientmap[0])
 {
 ci->clientmap[0] = '\0';
 ci->mapcrc = 0;
 }
 else if(ci->mapcrc > 0) ci->mapcrc = 0;
 break;
 }
 copystring(ci->clientmap, text);
 ci->mapcrc = text[0] ? crc : 1;
 checkmaps();
 break;
 }

 case N_CHECKMAPS:
 checkmaps(sender);
 break;

 case N_TRYSPAWN:
 if(!ci || !cq || cq->state.state!=CS_DEAD || cq->state.lastspawn>=0 || (smode && !smode->canspawn(cq))) break;
 if(!ci->clientmap[0] && !ci->mapcrc)
 {
 ci->mapcrc = -1;
 checkmaps();
 }
 if(cq->state.lastdeath)
 {
 flushevents(cq, cq->state.lastdeath + DEATHMILLIS);
 cq->state.respawn();
 }
 cleartimedevents(cq);
 sendspawn(cq);
 break;

 case N_GUNSELECT:
 {
 int gunselect = getint(p);
 if(!cq || cq->state.state!=CS_ALIVE || gunselect<GUN_FIST || gunselect>GUN_PISTOL) break;
 cq->state.gunselect = gunselect;
 QUEUE_AI;
 QUEUE_MSG;
 break;
 }

 case N_SPAWN:
 {
 int ls = getint(p), gunselect = getint(p);
 if(!cq || (cq->state.state!=CS_ALIVE && cq->state.state!=CS_DEAD) || ls!=cq->state.lifesequence || cq->state.lastspawn<0) break;
 cq->state.lastspawn = -1;
 cq->state.state = CS_ALIVE;
 cq->state.gunselect = gunselect;
 cq->exceeded = 0;
 if(smode) smode->spawned(cq);
 QUEUE_AI;
 QUEUE_BUF({
 putint(cm->messages, N_SPAWN);
 sendstate(cq->state, cm->messages);
 });
 break;
 }
 
 case N_SHOOT:
 {
 shotevent *shot = new shotevent;
 shot->id = getint(p);
 shot->millis = cq ? cq->geteventmillis(gamemillis, shot->id) : 0;
 shot->gun = getint(p);
 loopk(3) shot->from[k] = getint(p)/DMF;
 loopk(3) shot->to[k] = getint(p)/DMF;
 int hits = getint(p);
 loopk(hits)
 {
 if(p.overread()) break;
 hitinfo &hit = shot->hits.add();
 hit.target = getint(p);
 hit.lifesequence = getint(p);
 hit.dist = getint(p)/DMF;
 hit.rays = getint(p);
 loopk(3) hit.dir[k] = getint(p)/DNF;
 }
 if(cq)
 {
 cq->addevent(shot);
 cq->setpushed();
 }
 else delete shot;
 break;
 }

 case N_SUICIDE:
 {
 if(cq) cq->addevent(new suicideevent);
 luaCallback(LUAEVENT_SUICIDE, ci->clientnum);
 break;
 }

 case N_EXPLODE:
 {
 explodeevent *exp = new explodeevent;
 int cmillis = getint(p);
 exp->millis = cq ? cq->geteventmillis(gamemillis, cmillis) : 0;
 exp->gun = getint(p);
 exp->id = getint(p);
 int hits = getint(p);
 loopk(hits)
 {
 if(p.overread()) break;
 hitinfo &hit = exp->hits.add();
 hit.target = getint(p);
 hit.lifesequence = getint(p);
 hit.dist = getint(p)/DMF;
 hit.rays = getint(p);
 loopk(3) hit.dir[k] = getint(p)/DNF;
 }
 if(cq) cq->addevent(exp);
 else delete exp;
 break;
 }

 case N_ITEMPICKUP:
 {
 int n = getint(p);
 if(!cq) break;
 pickupevent *pickup = new pickupevent;
 pickup->ent = n;
 cq->addevent(pickup);
 break;
 }

 case N_TEXT:
 {
 getstring(text, p);
 filtertext(text, text);
 time_t rawtime;
 struct tm * timeinfo;
 char tad [80];
 char tadnc [80];
 time ( &rawtime );
 timeinfo = localtime ( &rawtime );
 //time format (hour:minute am/pm timezone dayname month day year)
 strftime (tad,80,"\f2%I:%M\f6%p \f1%Z \f4%A\f7, %B %d, %Y",timeinfo); 
 strftime (tadnc,80,"%I:%M%p %Z %A, %B %d, %Y",timeinfo); //time & date w/o color
 luaCallback(LUAEVENT_TEXT, ci->clientnum, 1, "s", text);
 if(getvar("chattoconsole")) {out(ECHO_CONSOLE, "(%s) %s: %s", tadnc, newstring(ci->name), newstring(text));}
 if(ci)
 {
 if(text[0] == '#' || text[0] == '@') {
 char *c = text;
 while(*c && isspace(*c)) c++;

//Admin Commands
if(textcmd("cmds", text) && ci->privilege) {
if(textcmd("help", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#help\nDescription: request help");break;}
if(textcmd("stats", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#stats (cn)\nDescription: View the current stats of a client");break;}
if(textcmd("revokepriv", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#revokepriv (cn)\nDescription: Wipe all the priviliges of a client");break;}
if(textcmd("me", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f7Usage: #me (message)\nDescription: Echo your name and message to all");break;}
if(textcmd("echo", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#echo (message)\nDescription: Echo your message to all");break;}
if(textcmd("pm", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#pm (cn) (message)\nDescription: Send a private message to another client");break;}
if(getvar("enablestopservercmd")) {if(textcmd("stopserver", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#stopserver (admin required)\nDescription: Stop the server");break;}}
if(textcmd("info", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#info\nDescription: View the current server info");break;}
if(textcmd("uptime", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#uptime\nDescription: Display the servers uptime");break;}
if(textcmd("killall", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#killall\nDescription: Frag everyone on the server");break;}
if(textcmd("forceintermission", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#forceintermission\nDescription: Force an intermission");break;}
if(textcmd("allowmaster", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#allowmaster 1/0\nDescription: Allow or disallow \"/setmaster 1\" command");break;}
if(textcmd("givemaster", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#givemaster (cn)\nDescription: Give master to another client");break;}
if(textcmd("ip", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#ip (cn)\nDescription: Get the IP of another client");break;}
if(textcmd("kick", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#kick (cn)\nDescription: Temporarily kick another client (reconnect possible immediatly)");break;}
if(textcmd("ban", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#ban (cn)\nDescription: Ban a client permanently");break;}
if(textcmd("frag", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#frag (cn)\nDescription: Suicide another client");break;}
if(textcmd("invadmin", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#invadmin (adminpass)\nDescription: Claim invisible admin");break;}
if(textcmd("callops", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#callops\nDescription: Call all available operators");break;}
if(textcmd("pausegame", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#pausegame 1/0\nDescription: Pause the current game");break;}
if(textcmd("getversion", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#getversion\nDescription: Get this server's QServ version");break;}
if(textcmd("persist", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#persist\nDescription: Enable persistant teams");break;}
if(textcmd("jump", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#jump\nDescription: Jump at normal height");break;}
if(ci->privilege == PRIV_MASTER) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Type \f1#cmds (command) \f7for information on a command\n\f4Privileged Commands \f0(Master)\f7: me, echo, time, stats, pm, givemaster, info, uptime, getversion, callops, forceintermission, kick and pausegame\nType \f2#cmds all \f7to return to this menu");};
if(ci->privilege == PRIV_ADMIN) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Type \f1#cmds (command) \f7for information on a command\n\f4Privileged Commands \f6(Admin)\f7: me, echo, time, stats, pm, givemaster, info, uptime, getversion, frag, callops, forceintermission, allowmaster, ip, invadmin, kick, ban, stopserver, pausegame, revokepriv, persist and jump\nType \f2#cmds all \f7to return to this menu");};
break;

//Public commands
} else if(textcmd("cmds", text)) {
    if(textcmd("help", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#help\nDescription: request help");break;}
    if(textcmd("stats", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#stats (cn)\nDescription: View the current stats of a client");break;}
    if(textcmd("me", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f7Usage: #me (message)\nDescription: Echo your name and message to all");break;}
    if(textcmd("echo", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#echo (message)\nDescription: Echo your message to all");break;}
    if(textcmd("pm", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#pm (cn) (message)\nDescription: Send a private message to another client");break;}
    if(getvar("enablestopservercmd")) {if(textcmd("stopserver", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#stopserver (admin required)\nDescription: Stop the server");break;}}
    if(textcmd("info", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#info\nDescription: View the current server info");break;}
    if(textcmd("uptime", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#uptime\nDescription: Display the servers uptime");break;}
    if(textcmd("killall", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#killall\nDescription: Frag everyone on the server");break;}
    if(textcmd("forceintermission", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#forceintermission\nDescription: Force an intermission");break;}
    if(textcmd("allowmaster", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#allowmaster 1/0\nDescription: Allow or disallow \"/setmaster 1\" command");break;}
    if(textcmd("givemaster", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#givemaster (cn)\nDescription: Give master to another client");break;}
    if(textcmd("ip", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#ip (cn)\nDescription: Get the IP of another client");break;}
    if(textcmd("kick", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#kick (cn)\nDescription: Temporarily kick another client (reconnect possible immediatly)");break;}
    if(textcmd("ban", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#ban (cn)\nDescription: Ban a client permanently");break;}
    if(textcmd("frag", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#frag (cn)\nDescription: Suicide another client");break;}
    if(textcmd("invadmin", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#invadmin (adminpass)\nDescription: Claim invisible admin");break;}
    if(textcmd("callops", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#callops\nDescription: Call all available operators");break;}
    if(textcmd("pausegame", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#pausegame 1/0\nDescription: Pause the current game");break;}
    if(textcmd("getversion", text+5)) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: \f7#getversion\nDescription: Get this server's QServ version");break;}
sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Type \f1#cmds (command) \f7for information on a command\n\f4Public Commands: \f7me, echo, time, invadmin, stats, pm, info, uptime, getversion and callops\nType \f2#cmds all \f7to return to this menu");
break;

//frags, flags, deaths, teamkills, shotdamage, damage
}else if(textcmd("stats", text)) {
if(text[6] == ' ') {
int cn1 = text[7] - '0'; //first int char
int cn2 = text[8] - '0'; //second int char
char CNc[255]; 

sprintf(CNc, "%d%d", cn1, cn2);
int iCN = atoi(CNc); //combined int's to make up CN

clientinfo *cn;
if(!isalpha(iCN)) {
if(iCN < 100) {
cn =(clientinfo *)getclientinfo(iCN);
}
}

if(cn != NULL) {
if (cn->connected && ci->privilege == PRIV_ADMIN){
defformatstring(s)("Stats for: \f0%s (%s) \n\f7Frags: \f0%i \f7Deaths: \f3%i \f7Teamkills: \f1%i \f7Flag Runs: \f2%i", colorname(cn), cn->ip, cn->state.frags, cn->state.deaths, cn->state.teamkills/2, cn->state.flags);
    sendf(ci->clientnum, 1, "ris", N_SERVMSG, s);
}
else {
defformatstring(s)("Stats for: \f0%s \n\f7Frags: \f0%i \f7Deaths: \f3%i \f7Teamkills: \f1%i \f7Flag Runs: \f2%i", colorname(cn), cn->state.frags, cn->state.deaths, cn->state.teamkills/2, cn->state.flags);
sendf(ci->clientnum, 1, "ris", N_SERVMSG, s);
}}
break;
}else if(text[6] == '\0') {
defformatstring(s)("\f0%s \n\f7Frags: \f0%i \f7Deaths: \f3%i \f7Teamkills: \f1%i \f7Flag Runs: \f2%i", colorname(ci), ci->state.frags, ci->state.deaths, ci->state.teamkills/2, ci->state.flags);
sendf(ci->clientnum, 1, "ris", N_SERVMSG, s);
}
break;

}else if(textcmd("jump", text) && ci->privilege == PRIV_ADMIN){
    sendf(ci->clientnum, 1, "ri7", N_HITPUSH, ci->clientnum, 1, 125, 0, 0, 125);
break;
}else if(textcmd("jump", text)) {
sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f3Error: \f7Insufficent permissions (admin required)");
break;
    
}else if(textcmd("time", text)){
    time_t rawtime;
    struct tm * timeinfo;
    char tad [80];
    char tadnc [80];
    time ( &rawtime );
    timeinfo = localtime ( &rawtime );
    //time format (hour:minute am/pm timezone dayname month day year)
    strftime (tad,80,"\f2%I:%M\f6%p \f1%Z \f4%A\f7, %B %d, %Y",timeinfo); 
    strftime (tadnc,80,"%I:%M%p %Z %A, %B %d, %Y",timeinfo); //time & date w/o color
    defformatstring(s)("%s", tad);
    sendf(ci->clientnum, 1, "ris", N_SERVMSG, s);
    puts(tadnc);
   break;

}else if(textcmd("help", text)){
    sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Use \f1#cmds \f7to list commands.\nIf you would like to speak with an operator, type \f3#callops");
    break;

}else if(textcmd("uptime", text)){
    ci->connectedmillis=(gamemillis/1000)+servuptime-(ci->connectmillis/1000);
    int ssstime = gamemillis+servuptime;
    int sssdays = ssstime/86400000, sssdaysms=sssdays*86400000;
    int ssshours = (ssstime-sssdaysms)/3600000, ssshoursms=ssshours*3600000;
    int sssminutes = (ssstime-sssdaysms-ssshoursms)/60000;
    int sssseconds = (ssstime-sssdaysms-ssshoursms-sssminutes*60000)/1000;
    
    defformatstring(f)("\f4Server Uptime: \f0%d Day(s)\f4, \f2%d Hour(s)\f4, \f1%d Minute(s)\f4, \f3%d Seconds", sssdays, ssshours, sssminutes, sssseconds);
    sendf(ci->clientnum, 1, "ris", N_SERVMSG, f);
    break;
    
}else if(textcmd("getversion", text)){
defformatstring(ver)("%s", qservversion);
sendf(ci->clientnum, 1, "ris", N_SERVMSG, ver);
break;
    
}else if(textcmd("getversion", text) && ci->privilege){
    defformatstring(ver)("%s", qservversion);
    sendf(ci->clientnum, 1, "ris", N_SERVMSG, ver);

}else if(textcmd("persist 1", text) && ci->privilege == PRIV_ADMIN){
persist = true;
defformatstring(s)("\f0%s \f7enabled \f4persistant teams", colorname(ci));
sendservmsg(s);
break;
}else if(textcmd("persist 0", text) && ci->privilege == PRIV_ADMIN){
persist = false;
defformatstring(s)("\f0%s \f7disabled \f4persistant teams", colorname(ci));
sendservmsg(s);
break;
}else if(textcmd("persist 1", text)){
 sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f3Error: \f7Insufficent permissions (admin required)");
break;
}else if(textcmd("persist 0", text)){
 sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f3Error: \f7Insufficent permissions (admin required)");
break;
}else if(textcmd("persist", text) && ci->privilege == PRIV_ADMIN){
 sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f3Error: \f7Persist option not declared (use #persist 1 or #persist 0)");
break;
}else if(textcmd("pausegame 1", text) && ci->privilege){
pausegame(true);
defformatstring(s)("\f0%s \f4Paused \f7the game", colorname(ci));
sendservmsg(s);
break;
}else if(textcmd("pausegame 0", text) && ci->privilege){
pausegame(false);
defformatstring(s)("\f0%s \f2Resumed \f7the game", colorname(ci));
sendservmsg(s);
break;
}else if(textcmd("pausegame 1", text)){
 sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f3Error: \f7Insufficent permissions (master required)");
break;
}else if(textcmd("pausegame 0", text)){
 sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f3Error: \f7Insufficent permissions (master required)");
break;
}else if(textcmd("pausegame", text) && ci->privilege){
 sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f3Error: \f7Pause or unpause not declared (use #pausegame 1 or #pausegame 0)");
break;

 }else if(textcmd("callops", text)){
defformatstring(s)("%s %s", callopmsg, operators); //callopmsg defined in server-init
sendf(ci->clientnum, 1, "ris", N_SERVMSG, s);
out(ECHO_CONSOLE, "Attention administrator(s), %s: %s (%s) needs assistance", operators, colorname(ci), ci->ip);
out(ECHO_IRC, "Attention operators %s: %s (%s) needs assistance", operators, colorname(ci), ci->ip);
break;

}else if(textcmd(invisadmin(), text)){ 
if(ci->privilege == PRIV_ADMIN) {break;}
else {
ci->privilege = PRIV_ADMIN;
sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Your privilege has been raised to \f1Admin \f7(invisible)");
out(ECHO_IRC, "%s claimed invisible admin", colorname(ci));
out(ECHO_CONSOLE, "%s claimed invisible admin", colorname(ci));
break;
 }
}else if(textcmd("invadmin", text)){
sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f3Error: \f7Invalid password");
break;

}else if(textcmd("ip", text) && ci->privilege == PRIV_ADMIN) {
if(text[3] == ' ' && !isalpha) {
int v = text[4] - '0';
clientinfo *cn = (clientinfo *)getclientinfo(v);
if(isalpha(text[4])) {sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f3Error: \f7An IP can only consist of numbers\n\f2Usage: \f7#ip (client number: numbers only)"); break;}
else if (cn->connected){
defformatstring(s)("IP of \f0%s\f7: \f1%s", colorname(cn), cn->ip);
sendf(ci->clientnum, 1, "ris", N_SERVMSG, s);
break;
}
}else if(text[3] == '\0') {
sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f2Usage: \f7#ip (cn)");
break;
}
}else if(textcmd("ip", text)) {
sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f3Error: \f7Insufficent permissions (admin required)");
break;

//Ban a client untill server is restarted
}else if(textcmd("ban", text) && ci->privilege == PRIV_ADMIN) {
if(text[4] == ' ' && !isalpha) {
int v = text[5] - '0';
clientinfo *cn = (clientinfo *)getclientinfo(v);
if (cn->connected){
ban &b = bannedips.add();
b.ip = getclientip(cn->clientnum);
b.time = gamemillis;
allowedips.removeobj(b.ip);
disconnect_client(cn->clientnum, DISC_BANNED);
break;
 }
}else if(text[4] == '\0') {
sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f2Usage: \f7#ban (cn)");
break;
}
}else if(textcmd("ban", text)) {
sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f3Error: \f7Insufficent permissions (admin required)");
break;

//Temporarily ban a client
}else if(textcmd("kick", text) && ci->privilege) {
if(text[5] == ' ' && !isalpha) {
int v = text[6] - '0';
clientinfo *cn = (clientinfo *)getclientinfo(v);
if (cn->connected){
disconnect_client(cn->clientnum, DISC_KICK);
break;
}
}else if(text[5] == '\0') {
sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f2Usage: \f7#kick (cn)");
break;
}
}else if(textcmd("kick", text)) {
sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f3Error: \f7Insufficent permissions (master required)");
break;
    
 }else if(textcmd("givemaster", text) && ci->privilege) {
 if(text[11] == ' ' && !isalpha) {
 int v = text[12] - '0';
clientinfo *cn = (clientinfo *)getclientinfo(v);
if (cn->connected){
ci->privilege=0;
currentmaster = cn->clientnum;
cn->privilege = PRIV_MASTER;
sendf(-1, 1, "ri4", N_CURRENTMASTER, currentmaster, currentmaster >= 0 ? cn->privilege : 0, mastermode);
defformatstring(b)("\f0%s \f7gave master to \f6%s", colorname(ci), colorname(cn));
sendservmsg(b);
out(ECHO_IRC, "%s gave master to %s", colorname(ci), colorname(cn));
out(ECHO_CONSOLE, "%s gave master to %s", colorname(ci), colorname(cn));
break;
}
}else if(text[11] == '\0') {
 sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f2Usage: \f7#givemaster (cn)");
break;
}
}else if(textcmd("givemaster", text)) {
sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f3Error: \f7Insufficent permissions (master required)");
break;

//Revoke Privilege from client
}else if(textcmd("revokepriv", text) && ci->privilege == PRIV_ADMIN) {
    if(text[11] == ' ' && !isalpha) {
        int v = text[12] - '0';
        clientinfo *cn = (clientinfo *)getclientinfo(v);
        if (cn->connected){
            revokemaster(cn);
            cn->privilege=0;
            cn->privilege = PRIV_NONE;
            sendf(-1, 1, "ri4", N_CURRENTMASTER, currentmaster, currentmaster >= 0 ? cn->privilege : 0, mastermode);
            sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Priviliges have been prevoked from the specified client", colorname(cn));
            break;
        }
    }else if(text[11] == '\0') {
        sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f2Usage: \f7#revokepriv (cn)");
        break;
    }
}else if(textcmd("revokepriv", text)) {
    sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f3Error: \f7Insufficent permissions (admin required)");

}else if(textcmd("forceintermission", text) && ci->privilege){
startintermission();
 break;
 }else if(textcmd("forceintermission", text)){
sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f3Error: \f7Insufficent permissions (master required)");
 break;

 }else if(textcmd("allowmaster 1", text) && ci->privilege == PRIV_ADMIN){
mastermask = MM_PRIVSERV;
out(ECHO_SERV, "Master has been \f0enabled");
out(ECHO_IRC, "Master has been enabled");
break;
}else if(textcmd("allowmaster 1", text)){
sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f3Error: \f7Insufficent permissions (admin required)");
break;

}else if(textcmd("allowmaster 0", text) && ci->privilege == PRIV_ADMIN){
mastermask = !MM_AUTOAPPROVE;
out(ECHO_SERV, "Master has been \f3disabled");
out(ECHO_IRC, "Master has been disabled");
break;
}else if(textcmd("allowmaster 0", text)){
sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f3Error: \f7Insufficent permissions (admin required)");
break;
}else if(textcmd("allowmaster", text)){
    sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f3Error: \f7Use \"#allowmaster 1\" to enable master and 0 to disable it");
    break;

}else if(textcmd("frag", text) && ci->privilege == PRIV_ADMIN) {
if(text[5] == ' ' && !isalpha) {
int v = text[6] - '0';
clientinfo *cn = (clientinfo *)getclientinfo(v);
if (cn->connected){
if(cn->state.state==CS_ALIVE) {
suicide(cn);
defformatstring(s)("\f0%s \f7suicided \f6%s", colorname(ci), colorname(cn));
sendservmsg(s);
out(ECHO_CONSOLE, "%s used the #frag command to suicide %s", colorname(ci), colorname(cn));
}
break;
}
}else if(text[5] == '\0') {
sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f2Usage: \f7#frag (cn)");
break;
}
}else if(textcmd("frag", text)) {
sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f3Error: \f7Insufficent permissions (master required)");
break;

/* Command crashes QServ
}else if(textcmd("killall", text) && ci->privilege){
loopv(clients) {
 clientinfo *t = clients[i];
 if(t->state.state==CS_ALIVE) {suicide(t);}
 }
 break;
}else if(textcmd("killall", text)){
sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f3Error: \f7Insufficent permissions (master required)");
 break;
*/
 
}else if(textcmd("me", text)) {
if(text[3] == ' ') {
defformatstring(s)("\f0%s\f7%s", ci->name, text+3);
sendservmsg(s);
out(ECHO_IRC, "%s %s", ci->name, text+3);
break;
}else if(text[3] == '\0') {
sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f2Usage: \f7#me (message)");
 break;

}
}else if(textcmd("echo", text)) {
if(text[5] == ' ') {
defformatstring(d)("\f7%s", text+6);
sendservmsg(d);
out(ECHO_IRC, "%s: %s", colorname(ci), text+6);
break;
}else if(text[5] == '\0') {
sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f2Usage: \f7#echo (mesage)");
break;
}

 }else if(textcmd("stopserver", text) && ci->privilege == PRIV_ADMIN && getvar("enablestopservercmd")){ 
out(ECHO_ALL, "%s stopped the server", colorname(ci));
 kicknonlocalclients();
 exit(EXIT_FAILURE);
 break;
}else if(textcmd("stopserver", text) && !getvar("enablestopservercmd")){
 sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f3Error: \f7You do not have authority to do this.\nOnly the Host can shut the server.");
 break;
 }else if(textcmd("stopserver", text)){
 sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f3Error: \f7Insufficent permissions (admin required)");
 break;

}else if(textcmd("info", text)){
    time_t rawtime;
    struct tm * timeinfo;
    char tad [80]; //time and date
    time ( &rawtime );
    timeinfo = localtime ( &rawtime );
    //time format (hour:minute am/pm timezone dayname month day year)
    strftime (tad,80,"\f2%I:%M\f6%p \f1%Z \f4%A\f7, %B %d, %Y",timeinfo); 
    char *clu = codelastupdated;
    char *servername = serverdesc;
    defformatstring(s)("%s \n%s \n%s", servername, tad, clu); //Servername, time and date and when code was last updated
    sendf(ci->clientnum, 1, "ris", N_SERVMSG, s);
    break;

 }else if(textcmd("pm", text)) {
 if(text[3] == ' ') {
 if(text[5] == ' '){
 int i = text[4] - '0';
 if (clients[i]->connected){
 defformatstring(d)("Your private message \f4\"%s\" \f7was securely sent to \f0%s", text+6, colorname(clients[i]));
 sendf(ci->clientnum, 1, "ris", N_SERVMSG, d);
 defformatstring(s)("\f4*** \f7Private message from \f0%s\f7:\f4%s", ci->name, text+5);
 sendf(i, 1, "ris", N_SERVMSG, s);
 out(ECHO_CONSOLE, "Private Message: %s -> %s: %s", colorname(clients[i]), colorname(ci), text+6); 
break;
 }else{
 sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f3Error: \f7Incorrect client specified");
 break;
 }
 }
 }else if(text[8] == '\0') {
 sendf(ci->clientnum, 1, "ris", N_SERVMSG, "Usage: #pm (cn) (desired message)");
 }

 }else if(text[1] == '#' || text[1] == '@') {
 QUEUE_AI;
 QUEUE_INT(N_TEXT);
 QUEUE_STR(text+1);
 break;

 }else{
 sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f3Error: \f7Command not recognized. \nUse \f1#cmds \f7for a list");
 break;
}
 }
out(ECHO_IRC, "%s: %s", newstring(ci->name), newstring(text));
for (int a=0; a<(strlen(*blkmsg)-1); a++) {
textblk(blkmsg[a], text, ci);
}
QUEUE_AI;
QUEUE_INT(N_TEXT);
QUEUE_STR(text);
 }
 break;
 }

case N_SAYTEAM:
 {
 getstring(text, p);
 out(ECHO_CONSOLE, "%s [team]: %s", newstring(ci->name), newstring(text));
 if(!ci || !cq || (ci->state.state==CS_SPECTATOR && !ci->local && !ci->privilege) || !m_teammode || !cq->team[0]) break;
 loopv(clients)
 {
 clientinfo *t = clients[i];
 if(t==cq || t->state.state==CS_SPECTATOR || t->state.aitype != AI_NONE || strcmp(cq->team, t->team)) continue;
 sendf(t->clientnum, 1, "riis", N_SAYTEAM, cq->clientnum, text);
 }
 break;
 }

 case N_SWITCHNAME:
 {
 QUEUE_MSG;
 getstring(text, p);
 filtertext(ci->name, text, false, MAXNAMELEN);
 if(!ci->name[0]) copystring(ci->name, "unnamed");
 QUEUE_STR(ci->name);
 break;
 }

 case N_SWITCHMODEL:
 {
 ci->playermodel = getint(p);
 QUEUE_MSG;
 break;
 }

 case N_SWITCHTEAM:
 {
 getstring(text, p);
 filtertext(text, text, false, MAXTEAMLEN);
 if(strcmp(ci->team, text) && m_teammode && (!smode || smode->canchangeteam(ci, ci->team, text)))
 {
 if(ci->state.state==CS_ALIVE) suicide(ci);
 copystring(ci->team, text);
 aiman::changeteam(ci);
 sendf(-1, 1, "riisi", N_SETTEAM, sender, ci->team, ci->state.state==CS_SPECTATOR ? -1 : 0);
 out(ECHO_CONSOLE, "%s switched to team %s", colorname(ci), ci->team);
 }
 break;
 }

 case N_MAPVOTE:
 case N_MAPCHANGE:
 {
 getstring(text, p);
 filtertext(text, text, false);
 int reqmode = getint(p);
 if(type!=N_MAPVOTE && !mapreload) break;
 vote(text, reqmode, sender);
 break;
 }

 case N_ITEMLIST:
 {
 if((ci->state.state==CS_SPECTATOR && !ci->privilege && !ci->local) || !notgotitems || strcmp(ci->clientmap, smapname)) { while(getint(p)>=0 && !p.overread()) getint(p); break; }
 int n;
 while((n = getint(p))>=0 && n<MAXENTS && !p.overread())
 {
 server_entity se = { NOTUSED, 0, false };
 while(sents.length()<=n) sents.add(se);
 sents[n].type = getint(p);
 if(canspawnitem(sents[n].type))
 {
 if(m_mp(gamemode) && delayspawn(sents[n].type)) sents[n].spawntime = spawntime(sents[n].type);
 else sents[n].spawned = true;
 }
 }
 notgotitems = false;
 break;
 }

 case N_EDITENT:
 {
 int i = getint(p);
 loopk(3) getint(p);
 int type = getint(p);
 loopk(5) getint(p);
 if(!ci || ci->state.state==CS_SPECTATOR) break;
 QUEUE_MSG;
 bool canspawn = canspawnitem(type);
 if(i<MAXENTS && (sents.inrange(i) || canspawnitem(type)))
 {
 server_entity se = { NOTUSED, 0, false };
 while(sents.length()<=i) sents.add(se);
 sents[i].type = type;
 if(canspawn ? !sents[i].spawned : (sents[i].spawned || sents[i].spawntime))
 {
 sents[i].spawntime = canspawn ? 1 : 0;
 sents[i].spawned = false;
 }
 }
 break;
 }

 case N_EDITVAR:
 {
 int type = getint(p);
 getstring(text, p);
 switch(type)
 {
 case ID_VAR: getint(p); break;
 case ID_FVAR: getfloat(p); break;
 case ID_SVAR: getstring(text, p);
 }
 if(ci && ci->state.state!=CS_SPECTATOR) QUEUE_MSG;
 break;
 }

 case N_PING: //Client->server keepalive heartbeat
 sendf(sender, 1, "i2", N_PONG, getint(p));
 break;

 case N_CLIENTPING:
 {
if((ci->ping > 550) && !ci->pingwarned) {defformatstring(s)("\f4Attention: \f0%s\f7, %s", colorname(ci), pingwarnmsg); sendservmsg(s); ci->pingwarned = true;}
 int ping = getint(p);
 if(ci)
 {
 ci->ping = ping;
 loopv(ci->bots) ci->bots[i]->ping = ping;
 }
 QUEUE_MSG;
 break;
 }

 case N_MASTERMODE:
 {
 int mm = getint(p);
 if((ci->privilege || ci->local) && mm>=MM_OPEN && mm<=MM_PRIVATE)
 {
 if((ci->privilege>=PRIV_ADMIN || ci->local) || (mastermask&(1<<mm)))
 {
 mastermode = mm;
 allowedips.shrink(0);
 if(mm>=MM_PRIVATE)
 {
if(numclients(-1,false)<=1){
defformatstring(i)("\f3Error: \f7You can't use private mode (3) because your the only person connected");
sendservmsg(i);
mastermode = MM_OPEN;
}
 loopv(clients) allowedips.add(getclientip(clients[i]->clientnum));
 }
 //sendf(-1, 1, "rii", N_MASTERMODE, mastermode);
 out(ECHO_SERV, "\f0%s \f7set mastermode to \f1%s \f7(\f4%d\f7)", colorname(ci), mastermodename(mastermode), mastermode);
out(ECHO_IRC, "%s set mastermode to %s (%d)", colorname(ci), mastermodename(mastermode), mastermode);
out(ECHO_CONSOLE, "%s set mastermode to %s (%d)", colorname(ci), mastermodename(mastermode), mastermode);
 }
 else
 {
 defformatstring(s)("\f3Error: Mastermode %d (%s) is disabled", mm, mastermodename(mastermode));
 sendf(sender, 1, "ris", N_SERVMSG, s);
 }
 }
 break;
 }

 case N_CLEARBANS:
 {  
     if(ci->privilege || ci->local) {bannedips.shrink(0);
         out(ECHO_SERV, "\f0%s \f7cleared all bans", colorname(ci));
         out(ECHO_CONSOLE, "%s cleared all bans", colorname(ci));
         out(ECHO_IRC, "All bans cleared");
     }
     else {
         sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f3Error: \f7Insufficent permissions to clear server bans");
     }
 break;
 }

 case N_KICK:
 {
 int victim = getint(p);
 if((ci->privilege || ci->local) && ci->clientnum!=victim && getclientinfo(victim)) // no bots
 {
 ban &b = bannedips.add();
 b.time = totalmillis;
 b.ip = getclientip(victim);
 allowedips.removeobj(b.ip);
 disconnect_client(victim, DISC_KICK);
 }
 break;
 }

 case N_SPECTATOR:
 {
 int spectator = getint(p), val = getint(p);
 if(!ci->privilege && !ci->local && (spectator!=sender || (ci->state.state==CS_SPECTATOR && mastermode>=MM_LOCKED))) break;
 clientinfo *spinfo = (clientinfo *)getclientinfo(spectator); // no bots
 if(!spinfo || (spinfo->state.state==CS_SPECTATOR ? val : !val)) break;

 if(spinfo->state.state!=CS_SPECTATOR && val)
{
 defformatstring(l)("\f0%s \f7is now a spectator", spinfo->name);
sendservmsg(l);
printf("%s is now a spectator\n", spinfo->name);
 if(spinfo->state.state==CS_ALIVE) suicide(spinfo);
 if(smode) smode->leavegame(spinfo);
 spinfo->state.state = CS_SPECTATOR;
 spinfo->state.timeplayed += lastmillis - spinfo->state.lasttimeplayed;
 if(!spinfo->local && !spinfo->privilege) aiman::removeai(spinfo);
 }
 else if(spinfo->state.state==CS_SPECTATOR && !val)
 {
defformatstring(l)("\f0%s \f7is no longer a spectator", spinfo->name);
sendservmsg(l);
printf("%s is no longer a spectator\n", spinfo->name);
 spinfo->state.state = CS_DEAD;
 spinfo->state.respawn();
 spinfo->state.lasttimeplayed = lastmillis;
 aiman::addclient(spinfo);
 if(spinfo->clientmap[0] || spinfo->mapcrc) checkmaps();
 sendf(-1, 1, "ri", N_MAPRELOAD);
 }
 sendf(-1, 1, "ri3", N_SPECTATOR, spectator, val);
 if(!val && mapreload && !spinfo->privilege && !spinfo->local) sendf(spectator, 1, "ri", N_MAPRELOAD);
 break;
 }

 case N_SETTEAM:
 {
 int who = getint(p);
 getstring(text, p);
 filtertext(text, text, false, MAXTEAMLEN);
 if(!ci->privilege && !ci->local) break;
 clientinfo *wi = getinfo(who);
 if(!wi || !strcmp(wi->team, text)) break;
 if(!smode || smode->canchangeteam(wi, wi->team, text))
 {
 if(wi->state.state==CS_ALIVE) suicide(wi);
 copystring(wi->team, text, MAXTEAMLEN+1);
 }
 aiman::changeteam(wi);
 sendf(-1, 1, "riisi", N_SETTEAM, who, wi->team, 1);
 break;
 }

 case N_FORCEINTERMISSION:
 if(ci->local && !hasnonlocalclients()) startintermission();
 break;

 case N_RECORDDEMO:
 {
 int val = getint(p);
 if(ci->privilege<PRIV_ADMIN && !ci->local) break;
 demonextmatch = val!=0;
 defformatstring(msg)("Demo recording has is %s for next map", demonextmatch ? "\f0enabled" : "\f3disabled");
 sendservmsg(msg);
out(ECHO_IRC, "\f0%s \f7has %s demo recording for the next map", colorname(ci), demonextmatch ? "\f0enabled" : "\f3disabled");
 break;
 }

 case N_STOPDEMO:
 {
 if(ci->privilege<PRIV_ADMIN && !ci->local) break;
 stopdemo();
 break;
 }

 case N_CLEARDEMOS:
 {
 int demo = getint(p);
 if(ci->privilege<PRIV_ADMIN && !ci->local) break;
 cleardemos(demo);
 break;
 }

 case N_LISTDEMOS:
 if(!ci->privilege && !ci->local && ci->state.state==CS_SPECTATOR) break;
 listdemos(sender);
 break;

 case N_GETDEMO:
 {
 int n = getint(p);
 if(!ci->privilege && !ci->local && ci->state.state==CS_SPECTATOR) break;
 senddemo(sender, n);
 break;
 }

 case N_GETMAP:
 if(mapdata)
 {
 sendfile(sender, 2, mapdata, "ri", N_SENDMAP);
 ci->needclipboard = totalmillis;
sendf(sender, 1, "ris", N_SERVMSG, "\f4Uploading current map from the server");
defformatstring(l)("\f0%s \f7is receiving map \f1\"%s\"", colorname(ci), smapname);
sendservmsg(l);
printf("%s is downloading the map...", colorname(ci));
 }
 else sendf(sender, 1, "ris", N_SERVMSG, "\f3Error: \f7No map to send (none previously uploaded)");
 break;

 case N_NEWMAP:
 {
 int size = getint(p);
 if(!ci->privilege && !ci->local && ci->state.state==CS_SPECTATOR) break;
 if(size>=0)
 {
 smapname[0] = '\0';
 resetitems();
 notgotitems = false;
 if(smode) smode->reset(true);
out(ECHO_SERV, "\f0%s \f7started a new map", colorname(ci));
out(ECHO_CONSOLE, "%s started a new map", colorname(ci));
 }
 QUEUE_MSG;
 break;
 }

 case N_SETMASTER:
 {
 int val = getint(p);
 getstring(text, p);
 setmaster(ci, val!=0, text);
 break;
 }

 case N_ADDBOT:
 {
 aiman::reqadd(ci, getint(p));
 break;
 }

 case N_DELBOT:
 {
 aiman::reqdel(ci);
 break;
 }

 case N_BOTLIMIT:
 {
 int limit = getint(p);
 if(ci) aiman::setbotlimit(ci, limit);
 break;
 }

 case N_BOTBALANCE:
 {
 int balance = getint(p);
 if(ci) aiman::setbotbalance(ci, balance!=0);
 break;
 }

 case N_AUTHTRY:
 {
 string desc, name;
 getstring(desc, p, sizeof(desc)); // unused for now
 getstring(name, p, sizeof(name));
 if(!desc[0]) tryauth(ci, name);
 break;
 }

 case N_AUTHANS:
 {
 string desc, ans;
 getstring(desc, p, sizeof(desc)); // unused for now
 uint id = (uint)getint(p);
 getstring(ans, p, sizeof(ans));
 if(!desc[0]) answerchallenge(ci, id, ans);
 break;
 }

 case N_PAUSEGAME:
 {
 int val = getint(p);
 if(ci->privilege<PRIV_MASTER && !ci->local) break;
 pausegame(val > 0);
 break;
 }

 case N_COPY:
 ci->cleanclipboard();
 ci->lastclipboard = totalmillis;
 goto genericmsg;

 case N_PASTE:
 if(ci->state.state!=CS_SPECTATOR) sendclipboard(ci);
 goto genericmsg;

 case N_CLIPBOARD:
 {
 int unpacklen = getint(p), packlen = getint(p);
 ci->cleanclipboard(false);
 if(ci->state.state==CS_SPECTATOR)
 {
 if(packlen > 0) p.subbuf(packlen);
 break;
 }
 if(packlen <= 0 || packlen > (1<<16) || unpacklen <= 0)
 {
 if(packlen > 0) p.subbuf(packlen);
 packlen = unpacklen = 0;
 }
 packetbuf q(32 + packlen, ENET_PACKET_FLAG_RELIABLE);
 putint(q, N_CLIPBOARD);
 putint(q, ci->clientnum);
 putint(q, unpacklen);
 putint(q, packlen);
 if(packlen > 0) p.get(q.subbuf(packlen).buf, packlen);
 ci->clipboard = q.finalize();
 ci->clipboard->referenceCount++;
 break;
 }

 #define PARSEMESSAGES 1
 #include "capture.h"
 #include "ctf.h"
 #undef PARSEMESSAGES

 case -1:
 disconnect_client(sender, DISC_TAGT);
 return;

 case -2:
 disconnect_client(sender, DISC_OVERFLOW);
 return;

 default: genericmsg:
 {
 int size = server::msgsizelookup(type);
 if(size<=0) { disconnect_client(sender, DISC_TAGT); return; }
 loopi(size-1) getint(p);
 if(ci && cq && (ci != cq || ci->state.state!=CS_SPECTATOR)) { QUEUE_AI; QUEUE_MSG; }
 break;
 }
 }
 }

 int laninfoport() { return SAUERBRATEN_LANINFO_PORT; }
 int serverinfoport(int servport) { return servport < 0 ? SAUERBRATEN_SERVINFO_PORT : servport+1; }
 int serverport(int infoport) { return infoport < 0 ? SAUERBRATEN_SERVER_PORT : infoport-1; }
 const char *defaultmaster() { return "sauerbraten.org"; }
 int masterport() { return SAUERBRATEN_MASTER_PORT; }
 int numchannels() { return 3; }

 #include "extinfo.h"

 void serverinforeply(ucharbuf &req, ucharbuf &p)
 {
 if(!getint(req))
 {
 extserverinforeply(req, p);
 return;
 }

 putint(p, numclients(-1, false, true));
 putint(p, 5); // number of attrs following
 putint(p, PROTOCOL_VERSION); // generic attributes, passed back below
 putint(p, gamemode);
 putint(p, max((gamelimit - gamemillis)/1000, 0));
 putint(p, maxclients);
 putint(p, serverpass[0] ? MM_PASSWORD : (!m_mp(gamemode) ? MM_PRIVATE : (mastermode || mastermask&MM_AUTOAPPROVE ? mastermode : MM_AUTH)));
 sendstring(smapname, p);
 sendstring(serverdesc, p);
 sendserverinforeply(p);
 }

 bool servercompatible(char *name, char *sdec, char *map, int ping, const vector<int> &attr, int np)
 {
 return attr.length() && attr[0]==PROTOCOL_VERSION;
 }

 #include "aiman.h"
}

#include "lunar.h"

#define ciNullCheck(what) \
 if(!ci) \
 return 0; \
 what; \
 return 1;

class clientinfo
{
public:
 static const char *className;
 static Lunar<clientinfo>::RegType methods[];

 clientinfo(lua_State *L) { ci = server::getinfo(luaL_checknumber(L, 1)); }

 int name(lua_State *L){ciNullCheck(lua_pushstring(L, ci->name));}
 int team(lua_State *L){ciNullCheck(lua_pushstring(L, ci->team));}
 int ip(lua_State *L){ciNullCheck(lua_pushstring(L, ci->ip));}
 int num(lua_State *L){ciNullCheck(lua_pushnumber(L, ci->clientnum));}
 int ping(lua_State *L){ciNullCheck(lua_pushnumber(L, ci->ping));}

 int frags(lua_State *L){ciNullCheck(lua_pushnumber(L, ci->state.frags));}
 int flags(lua_State *L){ciNullCheck(lua_pushnumber(L, ci->state.flags));}
 int deaths(lua_State *L){ciNullCheck(lua_pushnumber(L, ci->state.deaths));}
 int teamkills(lua_State *L){ciNullCheck(lua_pushnumber(L, ci->state.teamkills));}
 int shotdamage(lua_State *L){ciNullCheck(lua_pushnumber(L, ci->state.shotdamage));}
 int damage(lua_State *L){ciNullCheck(lua_pushnumber(L, ci->state.damage));}
 int privilege(lua_State *L){ciNullCheck(lua_pushnumber(L, ci->privilege));}

 ~clientinfo() { printf("deleted (%p)\n", this); }
 private:
 server::clientinfo *ci;
};

const char *clientinfo::className = "clientinfo";

Lunar<clientinfo>::RegType clientinfo::methods[] = {
 LUNAR_DECLARE_METHOD(clientinfo, name),
 LUNAR_DECLARE_METHOD(clientinfo, team),
 LUNAR_DECLARE_METHOD(clientinfo, ip),
 LUNAR_DECLARE_METHOD(clientinfo, num),
 LUNAR_DECLARE_METHOD(clientinfo, ping),
 LUNAR_DECLARE_METHOD(clientinfo, frags),
 LUNAR_DECLARE_METHOD(clientinfo, flags),
 LUNAR_DECLARE_METHOD(clientinfo, deaths),
 LUNAR_DECLARE_METHOD(clientinfo, teamkills),
 LUNAR_DECLARE_METHOD(clientinfo, shotdamage),
 LUNAR_DECLARE_METHOD(clientinfo, damage),
 LUNAR_DECLARE_METHOD(clientinfo, privilege),
 {0,0}
};

int initClientLib(lua_State *L)
{
 Lunar<clientinfo>::Register(L);
 return 1;
}
