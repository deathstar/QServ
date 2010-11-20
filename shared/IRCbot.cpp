#include "game.h"
#include "IRCbot.h"
#ifndef WIN32
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#endif

SVAR(irchost, "irc.freenode.net");
VAR(ircport, 0, 6667, 65535);
SVAR(ircchan, "#c2-server");
SVAR(ircbotname, "QServ");

ircBot irc;

ICOMMAND(clearbans, "", (), {
        server::clearbans();
});

ICOMMAND(echoall, "s", (char *s), {
        out(ECHO_ALL,s);
});
ICOMMAND(echoirc, "s", (char *s), {
        out(ECHO_IRC,s);
});
ICOMMAND(echocon, "s", (char *s), {
        out(ECHO_CONSOLE,s);
});
ICOMMAND(echoserv, "s", (char *s), {
        out(ECHO_SERV,s);
});

bool IsCommand(IrcMsg *msg)
{
    if(msg->message[0] == '#' || msg->message[0] == '@')
    {
        char *c = msg->message;
        c++;
        execute(c);
        return true;
    }return false;
}

int ircBot::getSock()
{
    return sock;
}

int ircBot::speak(const char *fmt, ...){
    char msg[1000], k[1000];
    va_list list;
    va_start(list,fmt);
    vsnprintf(k,1000,fmt,list);
    snprintf(msg,1000,"PRIVMSG %s :%s\r\n\0",ircchan,k);
    va_end(list);

    return send(sock,msg,strlen(msg),0);
}

void ircBot::ParseMessage(char *buff){
    if(sscanf(buff,":%[^!]!%[^@]@%[^ ] %*[^ ] %[^ :] :%[^\r\n]",msg.nick,msg.user,msg.host,msg.chan,msg.message) == 5){
        msg.is_ready = 1;
        if(msg.chan[0] != '#') strcpy(msg.chan,msg.nick);
    } else msg.is_ready = 0;
}

void ircBot::init()
{
    int con;
    struct sockaddr_in sa;
    struct hostent *he;

    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    memset(&sa, 0, sizeof sa);
    memset(&he, 0, sizeof he);
    sa.sin_family = AF_INET;
    he = gethostbyname(irchost);
    bcopy(*he->h_addr_list, (char *)&sa.sin_addr.s_addr, sizeof(sa.sin_addr.s_addr));
    sa.sin_port = htons(ircport);

    con = connect(sock, (struct sockaddr *)&sa, sizeof(sa));
    defformatstring(user)("USER %s 0 * %s\r\n", ircbotname, ircbotname);
    send(sock, user, strlen(user), 0);
    defformatstring(nick)("NICK %s\r\n", ircbotname);
    send(sock, nick, strlen(nick), 0);
    defformatstring(join)("JOIN %s\r\n", ircchan);
    send(sock, join, strlen(join), 0);
    int n;
    char mybuffer[1000];
    char Pout[30];
    while(1){
        n = recv(sock, mybuffer, sizeof(mybuffer), 0);
        puts(mybuffer);

        ParseMessage(mybuffer);

        if(sscanf(mybuffer,"PING: %s",mybuffer)==1){
            snprintf(Pout,30,"PONG: %s",Pout);
            send(sock,Pout,strlen(Pout),0);
        }
        if(!IsCommand(&msg)){
            defformatstring(toserver)("\f4%s \f3%s \f7- \f0%s\f7: %s", newstring(irchost), newstring(ircchan), msg.nick, msg.message);
            server::sendservmsg(toserver);
        }
        memset(mybuffer,'\0',1000);
        memset(Pout,'\0',30);
    }
}

void out(int type, char *fmt, ...)
{
    char msg[1000];
    va_list list;
    va_start(list,fmt);
    vsnprintf(msg,1000,fmt,list);
    va_end(list);

    switch(type)
    {
        case ECHO_ALL:
        {
            server::sendservmsg(msg);
            irc.speak(msg);
            puts(msg);
            break;
        }
        case ECHO_IRC:
        {
            irc.speak(msg);
            break;
        }
        case ECHO_CONSOLE:
        {
            puts(msg);
            break;
        }
        case ECHO_SERV:
        {
            server::sendservmsg(msg);
            break;
        }
        case ECHO_MASTER:
        {
            int ci = server::getmastercn();
            if(ci >= 0)
                sendf(ci, 1, "ris", N_SERVMSG, msg);
            break;
        }
        default:
            break;
    }
}
