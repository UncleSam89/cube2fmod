#include "game.h"

namespace game
{
    VARP(maxradarscale, 0, 1024, 10000);

    #include "capture.h"
    #include "ctf.h"

    clientmode *cmode = NULL;
    captureclientmode capturemode;
    ctfclientmode ctfmode;

    void setclientmode()
    {
        if(m_capture) cmode = &capturemode;
        else if(m_ctf) cmode = &ctfmode;
        else cmode = NULL;
    }

    bool senditemstoserver = false, sendcrc = false; // after a map change, since server doesn't have map data
    int lastping = 0;

    bool connected = false, remote = false, demoplayback = false, gamepaused = false;
    int sessionid = 0;
    string connectpass = "";

    VARP(deadpush, 1, 2, 20);

    void switchname(const char *name)
    {
        if(name[0])
        {
            filtertext(player1->name, name, false, MAXNAMELEN);
            if(!player1->name[0]) copystring(player1->name, "unnamed");
            addmsg(SV_SWITCHNAME, "rs", player1->name);
        }
        else conoutf("your name is: %s", colorname(player1));
    }
    ICOMMAND(name, "s", (char *s), switchname(s));
    ICOMMAND(getname, "", (), result(player1->name));

    void switchteam(const char *team)
    {
        if(team[0])
        {
            filtertext(player1->team, team, false, MAXTEAMLEN);
            addmsg(SV_SWITCHTEAM, "rs", player1->team);
        }
        else conoutf("your team is: %s", player1->team);
    }
    ICOMMAND(team, "s", (char *s), switchteam(s));
    ICOMMAND(getteam, "", (), result(player1->team));

    void switchplayermodel(int playermodel)
    {
        player1->playermodel = playermodel;
        addmsg(SV_SWITCHMODEL, "ri", player1->playermodel);
    }

    struct authkey
    {
        char *name, *key, *desc;
        int lastauth;

        authkey(const char *name, const char *key, const char *desc)
            : name(newstring(name)), key(newstring(key)), desc(newstring(desc)),
              lastauth(0)
        {
        }

        ~authkey()
        {
            DELETEA(name);
            DELETEA(key);
            DELETEA(desc);
        }
    };
    vector<authkey *> authkeys;

    authkey *findauthkey(const char *desc)
    {
        loopv(authkeys) if(!strcmp(authkeys[i]->desc, desc) && !strcmp(authkeys[i]->name, player1->name)) return authkeys[i];
        loopv(authkeys) if(!strcmp(authkeys[i]->desc, desc)) return authkeys[i];
        return NULL;
    }

    VARP(autoauth, 0, 1, 1);

    void addauthkey(const char *name, const char *key, const char *desc)
    {
        loopvrev(authkeys) if(!strcmp(authkeys[i]->desc, desc) && !strcmp(authkeys[i]->name, name)) delete authkeys.remove(i);
        if(name[0] && key[0]) authkeys.add(new authkey(name, key, desc));
    }
    ICOMMAND(authkey, "sss", (char *name, char *key, char *desc), addauthkey(name, key, desc));

    void genauthkey(const char *secret)
    {
        if(!secret[0]) { conoutf(CON_ERROR, "you must specify a secret password"); return; }
        vector<char> privkey, pubkey;
        genprivkey(secret, privkey, pubkey);
        conoutf("private key: %s", privkey.getbuf());
        conoutf("public key: %s", pubkey.getbuf());
    }
    COMMAND(genauthkey, "s");

    void saveauthkeys()
    {
        stream *f = openfile("auth.cfg", "w");
        if(!f) { conoutf(CON_ERROR, "failed to open auth.cfg for writing"); return; }
        loopv(authkeys)
        {
            authkey *a = authkeys[i];
            f->printf("authkey \"%s\" \"%s\" \"%s\"\n", a->name, a->key, a->desc);
        }
        conoutf("saved authkeys to auth.cfg");
        delete f;
    }
    COMMAND(saveauthkeys, "");

    int numchannels() { return 3; }

    void sendmapinfo()
    {
        sendcrc = true;
        if(player1->state!=CS_SPECTATOR || player1->privilege || !remote) senditemstoserver = true;
    }

    void writeclientinfo(stream *f)
    {
        f->printf("name \"%s\"\n", player1->name);
    }

    bool allowedittoggle()
    {
        bool allow = !connected || !multiplayer(false) || m_edit;
        if(!allow) conoutf(CON_ERROR, "editing in multiplayer requires coop edit mode (1)");
        return allow;
    }

    void edittoggled(bool on)
    {
        addmsg(SV_EDITMODE, "ri", on ? 1 : 0);
        if(player1->state==CS_DEAD) deathstate(player1, true);
        else if(player1->state==CS_EDITING && player1->editstate==CS_DEAD) showscores(false);
        disablezoom();
        player1->suicided = player1->respawned = -2;
    }

    const char *getclientname(int cn)
    {
        fpsent *d = getclient(cn);
        return d ? d->name : "";
    }
    ICOMMAND(getclientname, "i", (int *cn), result(getclientname(*cn)));

    const char *getclientteam(int cn)
    {
        fpsent *d = getclient(cn);
        return d ? d->team : "";
    }
    ICOMMAND(getclientteam, "i", (int *cn), result(getclientteam(*cn)));
    
    int getclientmodel(int cn)
    {
        fpsent *d = getclient(cn);
        return d ? d->playermodel : -1;
    }
    ICOMMAND(getclientmodel, "i", (int *cn), intret(getclientmodel(*cn)));

    const char *getclienticon(int cn)
    {
        fpsent *d = getclient(cn);
        if(!d || d->state==CS_SPECTATOR) return "spectator";
        const playermodelinfo &mdl = getplayermodelinfo(d);
        return m_teammode ? (isteam(player1->team, d->team) ? mdl.blueicon : mdl.redicon) : mdl.ffaicon;
    }
    ICOMMAND(getclienticon, "i", (int *cn), result(getclienticon(*cn)));

    bool isspectator(int cn)
    {
        fpsent *d = getclient(cn);
        return d->state==CS_SPECTATOR;
    }
    ICOMMAND(isspectator, "i", (int *cn), intret(isspectator(*cn) ? 1 : 0));

    int parseplayer(const char *arg)
    {
        char *end;
        int n = strtol(arg, &end, 10);
        if(*arg && !*end)
        {
            if(n!=player1->clientnum && !clients.inrange(n)) return -1;
            return n;
        }
        // try case sensitive first
        loopv(players)
        {
            fpsent *o = players[i];
            if(!strcmp(arg, o->name)) return o->clientnum;
        }
        // nothing found, try case insensitive
        loopv(players)
        {
            fpsent *o = players[i];
            if(!strcasecmp(arg, o->name)) return o->clientnum;
        }
        return -1;
    }
    ICOMMAND(getclientnum, "s", (char *name), intret(name[0] ? parseplayer(name) : player1->clientnum));

    void listclients(bool local)
    {
        vector<char> buf;
        string cn;
        int numclients = 0;
        if(local)
        {
            formatstring(cn)("%d", player1->clientnum);
            buf.put(cn, strlen(cn));
            numclients++;
        }
        loopv(clients) if(clients[i])
        {
            formatstring(cn)("%d", clients[i]->clientnum);
            if(numclients++) buf.add(' ');
            buf.put(cn, strlen(cn));
        }
        buf.add('\0');
        result(buf.getbuf());
    }
    ICOMMAND(listclients, "i", (int *local), listclients(*local!=0));

    void clearbans()
    {
        addmsg(SV_CLEARBANS, "r");
    }
    COMMAND(clearbans, "");

    void kick(const char *arg)
    {
        int i = parseplayer(arg);
        if(i>=0 && i!=player1->clientnum) addmsg(SV_KICK, "ri", i);
    }
    COMMAND(kick, "s");

    void setteam(const char *arg1, const char *arg2)
    {
        int i = parseplayer(arg1);
        if(i>=0 && i!=player1->clientnum) addmsg(SV_SETTEAM, "ris", i, arg2);
    }
    COMMAND(setteam, "ss");

    void hashpwd(const char *pwd)
    {
        if(player1->clientnum<0) return;
        string hash;
        server::hashpassword(player1->clientnum, sessionid, pwd, hash);
        result(hash);
    }
    COMMAND(hashpwd, "s");

    void setmaster(const char *arg)
    {
        if(!arg[0]) return;
        int val = 1;
        string hash = "";
        if(!arg[1] && isdigit(arg[0])) val = atoi(arg);
        else server::hashpassword(player1->clientnum, sessionid, arg, hash);
        addmsg(SV_SETMASTER, "ris", val, hash);
    }
    COMMAND(setmaster, "s");
    ICOMMAND(mastermode, "i", (int *val), addmsg(SV_MASTERMODE, "ri", *val));

    bool tryauth(const char *desc)
    {
        authkey *a = findauthkey(desc);
        if(!a) return false;
        a->lastauth = lastmillis;
        addmsg(SV_AUTHTRY, "rss", a->desc, a->name);
        return true;
    }
    ICOMMAND(auth, "s", (char *desc), tryauth(desc));

    void togglespectator(int val, const char *who)
    {
        int i = who[0] ? parseplayer(who) : player1->clientnum;
        if(i>=0) addmsg(SV_SPECTATOR, "rii", i, val);
    }
    ICOMMAND(spectator, "is", (int *val, char *who), togglespectator(*val, who));

    ICOMMAND(checkmaps, "", (), addmsg(SV_CHECKMAPS, "r"));

    VARP(localmode, STARTGAMEMODE, 1, STARTGAMEMODE + NUMGAMEMODES - 1);
    SVARP(localmap, "complex");
    VARP(lobbymode, STARTGAMEMODE, 0, STARTGAMEMODE + NUMGAMEMODES - 1);
    SVARP(lobbymap, "complex");

    int gamemode = INT_MAX, nextmode = INT_MAX;
    string clientmap = "";

    void changemapserv(const char *name, int mode)        // forced map change from the server
    {
        if(multiplayer(false) && !m_mp(mode))
        {
            conoutf(CON_ERROR, "mode %s (%d) not supported in multiplayer", server::modename(gamemode), gamemode);
            loopi(NUMGAMEMODES) if(m_mp(STARTGAMEMODE + i)) { mode = STARTGAMEMODE + i; break; }
        }

        gamemode = mode;
        nextmode = mode;
        minremain = -1;
        if(editmode) toggleedit();
        if(m_demo) { entities::resetspawns(); return; }
        if((m_edit && !name[0]) || !load_world(name))
        {
            emptymap(0, true, name);
            senditemstoserver = false;
        }
        startgame();
    }

    void setmode(int mode)
    {
        if(multiplayer(false) && !m_mp(mode))
        {
            conoutf(CON_ERROR, "mode %s (%d) not supported in multiplayer",  server::modename(mode), mode);
            intret(0);
            return;
        }
        nextmode = mode;
        intret(1);
    }
    ICOMMAND(mode, "i", (int *val), setmode(*val));
    ICOMMAND(getmode, "", (), intret(gamemode));

    void changemap(const char *name, int mode) // request map change, server may ignore
    {
        if(m_checknot(mode, M_EDIT) && !name[0])
            name = clientmap[0] ? clientmap : (remote ? lobbymap : localmap);
        if(!remote)
        {
            server::forcemap(name, mode);
            if(!connected) localconnect();
        }
        else if(player1->state!=CS_SPECTATOR || player1->privilege) addmsg(SV_MAPVOTE, "rsi", name, mode);
    }
    void changemap(const char *name)
    {
        changemap(name, m_valid(nextmode) ? nextmode : (remote ? lobbymode : localmode));
    }
    ICOMMAND(map, "s", (char *name), changemap(name));

    void forceedit(const char *name)
    {
        changemap(name, 1);
    }

    void newmap(int size)
    {
        addmsg(SV_NEWMAP, "ri", size);
    }

    void edittrigger(const selinfo &sel, int op, int arg1, int arg2, int arg3)
    {
        if(m_edit) switch(op)
        {
            case EDIT_FLIP:
            case EDIT_COPY:
            case EDIT_PASTE:
            case EDIT_DELCUBE:
            {
                addmsg(SV_EDITF + op, "ri9i4",
                   sel.o.x, sel.o.y, sel.o.z, sel.s.x, sel.s.y, sel.s.z, sel.grid, sel.orient,
                   sel.cx, sel.cxs, sel.cy, sel.cys, sel.corner);
                break;
            }
            case EDIT_ROTATE:
            {
                addmsg(SV_EDITF + op, "ri9i5",
                   sel.o.x, sel.o.y, sel.o.z, sel.s.x, sel.s.y, sel.s.z, sel.grid, sel.orient,
                   sel.cx, sel.cxs, sel.cy, sel.cys, sel.corner,
                   arg1);
                break;
            }
            case EDIT_MAT:
            case EDIT_FACE:
            case EDIT_TEX:
            case EDIT_REPLACE:
            {
                addmsg(SV_EDITF + op, "ri9i6",
                   sel.o.x, sel.o.y, sel.o.z, sel.s.x, sel.s.y, sel.s.z, sel.grid, sel.orient,
                   sel.cx, sel.cxs, sel.cy, sel.cys, sel.corner,
                   arg1, arg2);
                break;
            }
            case EDIT_REMIP:
            {
                addmsg(SV_EDITF + op, "r");
                break;
            }
        }
    }

    void printvar(fpsent *d, ident *id)
    {
        switch(id->type)
        {
            case ID_VAR:
            {
                int val = *id->storage.i;
                string str;
                if(id->flags&IDF_HEX && id->maxval==0xFFFFFF)
                    formatstring(str)("0x%.6X (%d, %d, %d)", val, (val>>16)&0xFF, (val>>8)&0xFF, val&0xFF);
                else
                    formatstring(str)(id->flags&IDF_HEX ? "0x%X" : "%d", val);
                conoutf("%s set map var \"%s\" to %s", colorname(d), id->name, str);
                break;
            }
            case ID_FVAR:
                conoutf("%s set map var \"%s\" to %s", colorname(d), id->name, floatstr(*id->storage.f));
                break;
            case ID_SVAR:
                conoutf("%s set map var \"%s\" to \"%s\"", colorname(d), id->name, *id->storage.s);
                break;
        }
    }

    void vartrigger(ident *id)
    {
        if(!m_edit) return;
        switch(id->type)
        {
            case ID_VAR:
                addmsg(SV_EDITVAR, "risi", ID_VAR, id->name, *id->storage.i);
                break;

            case ID_FVAR:
                addmsg(SV_EDITVAR, "risf", ID_FVAR, id->name, *id->storage.f);
                break;

            case ID_SVAR:
                addmsg(SV_EDITVAR, "riss", ID_SVAR, id->name, *id->storage.s);
                break;
            default: return;
        }
        printvar(player1, id);
    }

    void pausegame(int *val)
    {
        addmsg(SV_PAUSEGAME, "ri", *val > 0 ? 1 : 0);
    }
    COMMAND(pausegame, "i");

    bool ispaused() { return gamepaused; }

    // collect c2s messages conveniently
    vector<uchar> messages;
    int messagecn = -1, messagereliable = false;

    void addmsg(int type, const char *fmt, ...)
    {
        if(!connected) return;
        static uchar buf[MAXTRANS];
        ucharbuf p(buf, sizeof(buf));
        putint(p, type);
        int numi = 1, numf = 0, nums = 0, mcn = -1;
        bool reliable = false;
        if(fmt)
        {
            va_list args;
            va_start(args, fmt);
            while(*fmt) switch(*fmt++)
            {
                case 'r': reliable = true; break;
                case 'c':
                {
                    fpsent *d = va_arg(args, fpsent *);
                    mcn = !d || d == player1 ? -1 : d->clientnum;
                    break;
                }
                case 'v':
                {
                    int n = va_arg(args, int);
                    int *v = va_arg(args, int *);
                    loopi(n) putint(p, v[i]);
                    numi += n;
                    break;
                }

                case 'i':
                {
                    int n = isdigit(*fmt) ? *fmt++-'0' : 1;
                    loopi(n) putint(p, va_arg(args, int));
                    numi += n;
                    break;
                }
                case 'f':
                {
                    int n = isdigit(*fmt) ? *fmt++-'0' : 1;
                    loopi(n) putfloat(p, (float)va_arg(args, double));
                    numf += n;
                    break;
                }
                case 's': sendstring(va_arg(args, const char *), p); nums++; break;
            }
            va_end(args);
        }
        int num = nums || numf ? 0 : numi, msgsize = server::msgsizelookup(type);
        if(msgsize && num!=msgsize) { defformatstring(s)("inconsistent msg size for %d (%d != %d)", type, num, msgsize); fatal(s); }
        if(reliable) messagereliable = true;
        if(mcn != messagecn)
        {
            static uchar mbuf[16];
            ucharbuf m(mbuf, sizeof(mbuf));
            putint(m, SV_FROMAI);
            putint(m, mcn);
            messages.put(mbuf, m.length());
            messagecn = mcn;
        }
        messages.put(buf, p.length());
    }

    void connectattempt(const char *name, const char *password, const ENetAddress &address)
    {
        copystring(connectpass, password);
    }

    void connectfail()
    {
        memset(connectpass, 0, sizeof(connectpass));
    }

    void gameconnect(bool _remote)
    {
        connected = true;
        remote = _remote;
        if(editmode) toggleedit();
    }

    void gamedisconnect(bool cleanup)
    {
        if(remote) stopfollowing();
        connected = remote = false;
        player1->clientnum = -1;
        sessionid = 0;
        messages.setsizenodelete(0);
        messagereliable = false;
        messagecn = -1;
        player1->respawn();
        player1->lifesequence = 0;
        player1->state = CS_ALIVE;
        player1->privilege = PRIV_NONE;
        senditemstoserver = false;
        demoplayback = false;
        gamepaused = false;
        clearclients(false);
        if(cleanup)
        {
            nextmode = gamemode = INT_MAX;
            clientmap[0] = '\0';
        }
    }

    void toserver(char *text) { conoutf(CON_CHAT, "%s:\f0 %s", colorname(player1), text); addmsg(SV_TEXT, "rcs", player1, text); }
    COMMANDN(say, toserver, "C");

    void sayteam(char *text) { conoutf(CON_TEAMCHAT, "%s:\f1 %s", colorname(player1), text); addmsg(SV_SAYTEAM, "rcs", player1, text); }
    COMMAND(sayteam, "C");

    void sendposition(fpsent *d)
    {
        if(d->state != CS_ALIVE && d->state != CS_EDITING) return;
        packetbuf q(100);
        putint(q, SV_POS);
        putint(q, d->clientnum);
        putuint(q, (int)(d->o.x*DMF));              // quantize coordinates to 1/4th of a cube, between 1 and 3 bytes
        putuint(q, (int)(d->o.y*DMF));
        putuint(q, (int)((d->o.z-d->eyeheight)*DMF));
        putuint(q, (int)d->yaw);
        putint(q, (int)d->pitch);
        putint(q, (int)d->roll);
        putint(q, (int)(d->vel.x*DVELF));          // quantize to itself, almost always 1 byte
        putint(q, (int)(d->vel.y*DVELF));
        putint(q, (int)(d->vel.z*DVELF));
        uint physstate = d->physstate | ((d->lifesequence&1)<<6);
        if(d->falling.x || d->falling.y) physstate |= 0x20;
        if(d->falling.z) physstate |= 0x10;
        if((lookupmaterial(d->feetpos())&MATF_CLIP) == MAT_GAMECLIP) physstate |= 0x80;
        putuint(q, physstate);
        if(d->falling.x || d->falling.y)
        {
            putint(q, (int)(d->falling.x*DVELF));      // quantize to itself, almost always 1 byte
            putint(q, (int)(d->falling.y*DVELF));
        }
        if(d->falling.z) putint(q, (int)(d->falling.z*DVELF));
        // pack rest in almost always 1 byte: strafe:2, move:2, garmour: 1, yarmour: 1, quad: 1
        uint flags = (d->strafe&3) | ((d->move&3)<<2);
        putuint(q, flags);
        sendclientpacket(q.finalize(), 0);
    }

    void sendmessages(fpsent *d)
    {
        packetbuf p(MAXTRANS);
        if(sendcrc)
        {
            p.reliable();
            sendcrc = false;
            const char *mname = getclientmap();
            putint(p, SV_MAPCRC);
            sendstring(mname, p);
            putint(p, mname[0] ? getmapcrc() : 0);
        }
        if(senditemstoserver)
        {
            if(!m_noitems || cmode!=NULL) p.reliable();
            if(!m_noitems) entities::putitems(p);
            if(cmode) cmode->senditems(p);
            senditemstoserver = false;
        }
        if(messages.length())
        {
            p.put(messages.getbuf(), messages.length());
            messages.setsizenodelete(0);
            if(messagereliable) p.reliable();
            messagereliable = false;
            messagecn = -1;
        }
        if(lastmillis-lastping>250)
        {
            putint(p, SV_PING);
            putint(p, lastmillis);
            lastping = lastmillis;
        }
        sendclientpacket(p.finalize(), 1);
    }

    void c2sinfo() // send update to the server
    {
        static int lastupdate = -1000;
        if(totalmillis - lastupdate < 33) return;    // don't update faster than 30fps
        lastupdate = totalmillis;
        loopv(players)
        {
            fpsent *d = players[i];
            if(d == player1 || d->ai) sendposition(d);
        }
        sendmessages(player1);
        flushclient();
    }

    void sendintro()
    {
        packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
        putint(p, SV_CONNECT);
        sendstring(player1->name, p);
        string hash = "";
        if(connectpass[0])
        {
            server::hashpassword(player1->clientnum, sessionid, connectpass, hash);
            memset(connectpass, 0, sizeof(connectpass));
        }
        sendstring(hash, p);
        putint(p, player1->playermodel);
        sendclientpacket(p.finalize(), 1);
    }

    void updatepos(fpsent *d)
    {
        // update the position of other clients in the game in our world
        // don't care if he's in the scenery or other players,
        // just don't overlap with our client

        const float r = player1->radius+d->radius;
        const float dx = player1->o.x-d->o.x;
        const float dy = player1->o.y-d->o.y;
        const float dz = player1->o.z-d->o.z;
        const float rz = player1->aboveeye+d->eyeheight;
        const float fx = (float)fabs(dx), fy = (float)fabs(dy), fz = (float)fabs(dz);
        if(fx<r && fy<r && fz<rz && player1->state!=CS_SPECTATOR && d->state!=CS_DEAD)
        {
            if(fx<fy) d->o.y += dy<0 ? r-fy : -(r-fy);  // push aside
            else      d->o.x += dx<0 ? r-fx : -(r-fx);
        }
        int lagtime = lastmillis-d->lastupdate;
        if(lagtime)
        {
            if(d->state!=CS_SPAWNING && d->lastupdate) d->plag = (d->plag*5+lagtime)/6;
            d->lastupdate = lastmillis;
        }
    }

    void parsepositions(ucharbuf &p)
    {
        int type;
        while(p.remaining()) switch(type = getint(p))
        {
            case SV_POS:                        // position of another client
            {
                int cn = getint(p);
                vec o, vel, falling;
                float yaw, pitch, roll;
                int physstate, f;
                o.x = getuint(p)/DMF;
                o.y = getuint(p)/DMF;
                o.z = getuint(p)/DMF;
                yaw = (float)getuint(p);
                pitch = (float)getint(p);
                roll = (float)getint(p);
                vel.x = getint(p)/DVELF;
                vel.y = getint(p)/DVELF;
                vel.z = getint(p)/DVELF;
                physstate = getuint(p);
                falling = vec(0, 0, 0);
                if(physstate&0x20)
                {
                    falling.x = getint(p)/DVELF;
                    falling.y = getint(p)/DVELF;
                }
                if(physstate&0x10) falling.z = getint(p)/DVELF;
                int seqcolor = (physstate>>6)&1;
                f = getuint(p);
                fpsent *d = getclient(cn);
                if(!d || d->lifesequence < 0 || seqcolor!=(d->lifesequence&1) || d->state==CS_DEAD) continue;
                float oldyaw = d->yaw, oldpitch = d->pitch;
                d->yaw = yaw;
                d->pitch = pitch;
                d->roll = roll;
                d->strafe = (f&3)==3 ? -1 : f&3;
                f >>= 2;
                d->move = (f&3)==3 ? -1 : f&3;
                vec oldpos(d->o);
                if(allowmove(d))
                {
                    d->o = o;
                    d->o.z += d->eyeheight;
                    d->vel = vel;
                    d->falling = falling;
                    d->physstate = physstate & 0x0F;
                    updatephysstate(d);
                    updatepos(d);
                }
                if(smoothmove && d->smoothmillis>=0 && oldpos.dist(d->o) < smoothdist)
                {
                    d->newpos = d->o;
                    d->newyaw = d->yaw;
                    d->newpitch = d->pitch;
                    d->o = oldpos;
                    d->yaw = oldyaw;
                    d->pitch = oldpitch;
                    (d->deltapos = oldpos).sub(d->newpos);
                    d->deltayaw = oldyaw - d->newyaw;
                    if(d->deltayaw > 180) d->deltayaw -= 360;
                    else if(d->deltayaw < -180) d->deltayaw += 360;
                    d->deltapitch = oldpitch - d->newpitch;
                    d->smoothmillis = lastmillis;
                }
                else d->smoothmillis = 0;
                if(d->state==CS_LAGGED || d->state==CS_SPAWNING) d->state = CS_ALIVE;
                break;
            }

            default:
                neterr("type");
                return;
        }
    }

    void parsestate(fpsent *d, ucharbuf &p, bool resume = false)
    {
        if(!d) { static fpsent dummy; d = &dummy; }
        if(resume)
        {
            if(d==player1) getint(p);
            else d->state = getint(p);
            d->frags = getint(p);
            if(d==player1) getint(p);
            else d->quadmillis = getint(p);
        }
        d->lifesequence = getint(p);
        d->health = getint(p);
        d->maxhealth = getint(p);
        d->armour = getint(p);
        d->armourtype = getint(p);
        if(resume && d==player1)
        {
            getint(p);
            loopi(GUN_PISTOL-GUN_SG+1) getint(p);
        }
        else
        {
            d->gunselect = getint(p);
            loopi(GUN_PISTOL-GUN_SG+1) d->ammo[GUN_SG+i] = getint(p);
        }
    }

    void parsemessages(int cn, fpsent *d, ucharbuf &p)
    {
        static char text[MAXTRANS];
        int type;
        bool mapchanged = false, initmap = false;

        while(p.remaining()) switch(type = getint(p))
        {
            case SV_SERVINFO:                   // welcome messsage from the server
            {
                int mycn = getint(p), prot = getint(p);
                if(prot!=PROTOCOL_VERSION)
                {
                    conoutf(CON_ERROR, "you are using a different game protocol (you: %d, server: %d)", PROTOCOL_VERSION, prot);
                    disconnect();
                    return;
                }
                sessionid = getint(p);
                player1->clientnum = mycn;      // we are now connected
                if(getint(p) > 0) conoutf("this server is password protected");
                sendintro();
                break;
            }

            case SV_WELCOME:
            {
                int hasmap = getint(p);
                if(!hasmap) initmap = true; // we are the first client on this server, set map
                break;
            }

            case SV_PAUSEGAME:
            {
                int val = getint(p);
                gamepaused = val > 0;
                conoutf("game is %s", gamepaused ? "paused" : "resumed");
                break;
            }

            case SV_CLIENT:
            {
                int cn = getint(p), len = getuint(p);
                ucharbuf q = p.subbuf(len);
                parsemessages(cn, getclient(cn), q);
                break;
            }

            case SV_SOUND:
                if(!d) return;
                playsound(getint(p), &d->o);
                break;

            case SV_TEXT:
            {
                if(!d) return;
                getstring(text, p);
                filtertext(text, text);
                if(d->state!=CS_DEAD && d->state!=CS_SPECTATOR)
                {
                    defformatstring(ds)("@%s", &text);
                    particle_text(d->abovehead(), ds, PART_TEXT, 2000, 0x32FF64, 4.0f, -8);
                }
                conoutf(CON_CHAT, "%s:\f0 %s", colorname(d), text);
                break;
            }

            case SV_SAYTEAM:
            {
                int tcn = getint(p);
                fpsent *t = getclient(tcn);
                getstring(text, p);
                filtertext(text, text);
                if(!t) break;
                if(t->state!=CS_DEAD && t->state!=CS_SPECTATOR)
                {
                    defformatstring(ts)("@%s", &text);
                    particle_text(t->abovehead(), ts, PART_TEXT, 2000, 0x6496FF, 4.0f, -8);
                }
                conoutf(CON_TEAMCHAT, "%s:\f1 %s", colorname(t), text);
                break;
            }

            case SV_MAPCHANGE:
                getstring(text, p);
                changemapserv(text, getint(p));
                mapchanged = true;
                if(getint(p)) entities::spawnitems();
                else senditemstoserver = false;
                break;

            case SV_FORCEDEATH:
            {
                int cn = getint(p);
                fpsent *d = cn==player1->clientnum ? player1 : newclient(cn);
                if(!d) break;
                if(d==player1)
                {
                    if(editmode) toggleedit();
                    stopfollowing();
                    showscores(true);
                }
                else d->resetinterp();
                d->state = CS_DEAD;
                break;
            }

            case SV_ITEMLIST:
            {
                int n;
                while((n = getint(p))>=0 && !p.overread())
                {
                    if(mapchanged) entities::setspawn(n, true);
                    getint(p); // type
                }
                break;
            }

            case SV_MAPRELOAD:          // server requests next map
            {
                defformatstring(nextmapalias)("nextmap_%s%s", (cmode ? cmode->prefixnextmap() : ""), getclientmap());
                const char *map = getalias(nextmapalias);     // look up map in the cycle
                addmsg(SV_MAPCHANGE, "rsi", *map ? map : getclientmap(), nextmode);
                break;
            }

            case SV_INITCLIENT:            // another client either connected or changed name/team
            {
                int cn = getint(p);
                fpsent *d = newclient(cn);
                if(!d)
                {
                    getstring(text, p);
                    getstring(text, p);
                    getint(p);
                    break;
                }
                getstring(text, p);
                filtertext(text, text, false, MAXNAMELEN);
                if(!text[0]) copystring(text, "unnamed");
                if(d->name[0])          // already connected
                {
                    if(strcmp(d->name, text))
                        conoutf("%s is now known as %s", colorname(d), colorname(d, text));
                }
                else                    // new client
                {
                    conoutf("connected: %s", colorname(d, text));
                    loopv(players)   // clear copies since new player doesn't have them
                        freeeditinfo(players[i]->edit);
                    freeeditinfo(localedit);
                }
                copystring(d->name, text, MAXNAMELEN+1);
                getstring(text, p);
                filtertext(d->team, text, false, MAXTEAMLEN);
                d->playermodel = getint(p);
                break;
            }

            case SV_SWITCHNAME:
                getstring(text, p);
                if(d)
                {
                    filtertext(text, text, false, MAXNAMELEN);
                    if(!text[0]) copystring(text, "unnamed");
                    conoutf("%s is now known as %s", colorname(d), colorname(d, text));
                    copystring(d->name, text, MAXNAMELEN+1);
                }
                break;

            case SV_SWITCHMODEL:
            {
                int model = getint(p);
                if(d) d->playermodel = model;
                break;
            }

            case SV_CDIS:
                clientdisconnected(getint(p));
                break;

            case SV_SPAWN:
            {
                if(d)
                {
                    if(d->state==CS_DEAD && d->lastpain) saveragdoll(d);
                    d->respawn();
                }
                parsestate(d, p);
                if(!d) break;
                d->state = CS_SPAWNING;
                if(player1->state==CS_SPECTATOR && following==d->clientnum)
                    lasthit = 0;
                break;
            }

            case SV_SPAWNSTATE:
            {
                int scn = getint(p);
                fpsent *s = getclient(scn);
                if(!s) { parsestate(NULL, p); break; }
                if(s->state==CS_DEAD && s->lastpain) saveragdoll(s);
                if(s==player1)
                {
                    if(editmode) toggleedit();
                    stopfollowing();
                }
                s->respawn();
                parsestate(s, p);
                s->state = CS_ALIVE;
                if(cmode) cmode->pickspawn(s);
                else findplayerspawn(s);
                if(s == player1)
                {
                    showscores(false);
                    lasthit = 0;
                }
                if(cmode) cmode->respawned(s);
				ai::spawned(s);
                addmsg(SV_SPAWN, "rcii", s, s->lifesequence, s->gunselect);
                break;
            }

            case SV_SHOTFX:
            {
                int scn = getint(p), gun = getint(p);
                vec from, to;
                loopk(3) from[k] = getint(p)/DMF;
                loopk(3) to[k] = getint(p)/DMF;
                fpsent *s = getclient(scn);
                if(!s) break;
                if(gun>GUN_FIST && gun<=GUN_PISTOL && s->ammo[gun]) s->ammo[gun]--;
                s->gunselect = clamp(gun, (int)GUN_FIST, (int)GUN_PISTOL);
                s->gunwait = guns[s->gunselect].attackdelay;
                int prevaction = s->lastaction;
                s->lastaction = lastmillis;
                s->lastattackgun = s->gunselect;
                shoteffects(gun, from, to, s, false, prevaction);
                break;
            }

            case SV_DAMAGE:
            {
                int tcn = getint(p),
                    acn = getint(p),
                    damage = getint(p),
                    armour = getint(p),
                    health = getint(p);
                fpsent *target = getclient(tcn),
                       *actor = getclient(acn);
                if(!target || !actor) break;
                target->armour = armour;
                target->health = health;
                damaged(damage, target, actor, false);
                break;
            }

            case SV_HITPUSH:
            {
                int tcn = getint(p), gun = getint(p), damage = getint(p);
                fpsent *target = getclient(tcn);
                vec dir;
                loopk(3) dir[k] = getint(p)/DNF;
                if(target) target->hitpush(damage * (target->health<=0 ? deadpush : 1), dir, NULL, gun);
                break;
            }

            case SV_DIED:
            {
                int vcn = getint(p), acn = getint(p), frags = getint(p);
                fpsent *victim = getclient(vcn),
                       *actor = getclient(acn);
                if(!actor) break;
                actor->frags = frags;
                if(actor!=player1 && (!cmode || !cmode->hidefrags()))
                {
                    defformatstring(ds)("@%d", actor->frags);
                    particle_text(actor->abovehead(), ds, PART_TEXT, 2000, 0x32FF64, 4.0f, -8);
                }
                if(!victim) break;
                killed(victim, actor);
                break;
            }

            case SV_GUNSELECT:
            {
                if(!d) return;
                int gun = getint(p);
                d->gunselect = max(gun, 0);
                playsound(S_WEAPLOAD, &d->o);
                break;
            }

            case SV_TAUNT:
            {
                if(!d) return;
                d->lasttaunt = lastmillis;
                break;
            }

            case SV_RESUME:
            {
                for(;;)
                {
                    int cn = getint(p);
                    if(p.overread() || cn<0) break;
                    fpsent *d = (cn == player1->clientnum ? player1 : newclient(cn));
                    parsestate(d, p, true);
                }
                break;
            }

            case SV_ITEMSPAWN:
            {
                int i = getint(p);
                if(!entities::ents.inrange(i)) break;
                entities::setspawn(i, true);
                playsound(S_ITEMSPAWN, &entities::ents[i]->o);
                const char *name = entities::itemname(i);
                if(name) particle_text(entities::ents[i]->o, name, PART_TEXT, 2000, 0x32FF64, 4.0f, -8);
                break;
            }

            case SV_ITEMACC:            // server acknowledges that I picked up this item
            {
                int i = getint(p), cn = getint(p);
                fpsent *d = getclient(cn);
                entities::pickupeffects(i, d);
                break;
            }

            case SV_EDITF:              // coop editing messages
            case SV_EDITT:
            case SV_EDITM:
            case SV_FLIP:
            case SV_COPY:
            case SV_PASTE:
            case SV_ROTATE:
            case SV_REPLACE:
            case SV_DELCUBE:
            {
                if(!d) return;
                selinfo sel;
                sel.o.x = getint(p); sel.o.y = getint(p); sel.o.z = getint(p);
                sel.s.x = getint(p); sel.s.y = getint(p); sel.s.z = getint(p);
                sel.grid = getint(p); sel.orient = getint(p);
                sel.cx = getint(p); sel.cxs = getint(p); sel.cy = getint(p), sel.cys = getint(p);
                sel.corner = getint(p);
                int dir, mode, tex, newtex, mat, filter, allfaces;
                ivec moveo;
                switch(type)
                {
                    case SV_EDITF: dir = getint(p); mode = getint(p); mpeditface(dir, mode, sel, false); break;
                    case SV_EDITT: tex = getint(p); allfaces = getint(p); mpedittex(tex, allfaces, sel, false); break;
                    case SV_EDITM: mat = getint(p); filter = getint(p); mpeditmat(mat, filter, sel, false); break;
                    case SV_FLIP: mpflip(sel, false); break;
                    case SV_COPY: if(d) mpcopy(d->edit, sel, false); break;
                    case SV_PASTE: if(d) mppaste(d->edit, sel, false); break;
                    case SV_ROTATE: dir = getint(p); mprotate(dir, sel, false); break;
                    case SV_REPLACE: tex = getint(p); newtex = getint(p); mpreplacetex(tex, newtex, sel, false); break;
                    case SV_DELCUBE: mpdelcube(sel, false); break;
                }
                break;
            }
            case SV_REMIP:
            {
                if(!d) return;
                conoutf("%s remipped", colorname(d));
                mpremip(false);
                break;
            }
            case SV_EDITENT:            // coop edit of ent
            {
                if(!d) return;
                int i = getint(p);
                float x = getint(p)/DMF, y = getint(p)/DMF, z = getint(p)/DMF;
                int type = getint(p);
                int attr1 = getint(p), attr2 = getint(p), attr3 = getint(p), attr4 = getint(p), attr5 = getint(p);

                mpeditent(i, vec(x, y, z), type, attr1, attr2, attr3, attr4, attr5, false);
                break;
            }
            case SV_EDITVAR:
            {
                if(!d) return;
                int type = getint(p);
                getstring(text, p);
                string name;
                filtertext(name, text, false, MAXSTRLEN-1);
                ident *id = getident(name);
                switch(type)
                {
                    case ID_VAR:
                    {
                        int val = getint(p);
                        if(id && !(id->flags&IDF_READONLY)) setvar(name, val);
                        break;
                    }
                    case ID_FVAR:
                    {
                        float val = getfloat(p);
                        if(id && !(id->flags&IDF_READONLY)) setfvar(name, val);
                        break;
                    }
                    case ID_SVAR:
                    {
                        getstring(text, p);
                        if(id && !(id->flags&IDF_READONLY)) setsvar(name, text);
                        break;
                    }
                }
                printvar(d, id);
                break;
            }

            case SV_PONG:
                addmsg(SV_CLIENTPING, "i", player1->ping = (player1->ping*5+lastmillis-getint(p))/6);
                break;

            case SV_CLIENTPING:
                if(!d) return;
                d->ping = getint(p);
                break;

            case SV_TIMEUP:
                timeupdate(getint(p));
                break;

            case SV_SERVMSG:
                getstring(text, p);
                conoutf("%s", text);
                break;

            case SV_SENDDEMOLIST:
            {
                int demos = getint(p);
                if(!demos) conoutf("no demos available");
                else loopi(demos)
                {
                    getstring(text, p);
                    conoutf("%d. %s", i+1, text);
                }
                break;
            }

            case SV_DEMOPLAYBACK:
            {
                int on = getint(p);
                if(on) player1->state = CS_SPECTATOR;
                else clearclients();
                demoplayback = on!=0;
                player1->clientnum = getint(p);
                gamepaused = false;
                const char *alias = on ? "demostart" : "demoend";
                if(identexists(alias)) execute(alias);
                break;
            }

            case SV_CURRENTMASTER:
            {
                int mn = getint(p), priv = getint(p);
                loopv(players) players[i]->privilege = PRIV_NONE;
                if(mn>=0)
                {
                    fpsent *m = mn==player1->clientnum ? player1 : newclient(mn);
                    if(m) m->privilege = priv;
                }
                break;
            }

            case SV_EDITMODE:
            {
                int val = getint(p);
                if(!d) break;
                if(val)
                {
                    d->editstate = d->state;
                    d->state = CS_EDITING;
                }
                else
                {
                    d->state = d->editstate;
                    if(d->state==CS_DEAD) deathstate(d, true);
                }
                break;
            }

            case SV_SPECTATOR:
            {
                int sn = getint(p), val = getint(p);
                fpsent *s;
                if(sn==player1->clientnum)
                {
                    s = player1;
                    if(val && remote && !player1->privilege) senditemstoserver = false;
                }
                else s = newclient(sn);
                if(!s) return;
                if(val)
                {
                    if(s==player1)
                    {
                        if(editmode) toggleedit();
                        if(s->state==CS_DEAD) showscores(false);
                        disablezoom();
                    }
                    s->state = CS_SPECTATOR;
                }
                else if(s->state==CS_SPECTATOR)
                {
                    if(s==player1) stopfollowing();
                    deathstate(s, true);
                }
                break;
            }

            case SV_SETTEAM:
            {
                int wn = getint(p);
                getstring(text, p);
                fpsent *w = getclient(wn);
                if(!w) return;
                filtertext(w->team, text, false, MAXTEAMLEN);
                break;
            }

            #define PARSEMESSAGES 1
            #include "capture.h"
            #include "ctf.h"
            #undef PARSEMESSAGES

            case SV_ANNOUNCE:
            {
                int t = getint(p);
                if     (t==I_QUAD)  { playsound(S_V_QUAD10);  conoutf(CON_GAMEINFO, "\f2quad damage will spawn in 10 seconds!"); }
                else if(t==I_BOOST) { playsound(S_V_BOOST10); conoutf(CON_GAMEINFO, "\f2+10 health will spawn in 10 seconds!"); }
                break;
            }

            case SV_NEWMAP:
            {
                int size = getint(p);
                if(size>=0) emptymap(size, true, NULL);
                else enlargemap(true);
                if(d && d!=player1)
                {
                    int newsize = 0;
                    while(1<<newsize < getworldsize()) newsize++;
                    conoutf(size>=0 ? "%s started a new map of size %d" : "%s enlarged the map to size %d", colorname(d), newsize);
                }
                break;
            }

            case SV_REQAUTH:
            {
                getstring(text, p);
                if(autoauth && text[0] && tryauth(text)) conoutf("server requested authkey \"%s\"", text);
                break;
            }

            case SV_AUTHCHAL:
            {
                getstring(text, p);
                authkey *a = findauthkey(text);
                uint id = (uint)getint(p);
                getstring(text, p);
                if(a && a->lastauth && lastmillis - a->lastauth < 60*1000)
                {
                    vector<char> buf;
                    answerchallenge(a->key, text, buf);
                    //conoutf(CON_DEBUG, "answering %u, challenge %s with %s", id, text, buf.getbuf());
                    addmsg(SV_AUTHANS, "rsis", a->desc, id, buf.getbuf());
                }
                break;
            }

            case SV_INITAI:
            {
                int bn = getint(p), on = getint(p), at = getint(p), sk = clamp(getint(p), 1, 101), pm = getint(p);
                string name, team;
                getstring(text, p);
                filtertext(name, text, false, MAXNAMELEN);
                getstring(text, p);
                filtertext(team, text, false, MAXTEAMLEN);
                fpsent *b = newclient(bn);
                if(!b) break;
                ai::init(b, at, on, sk, bn, pm, name, team);
                break;
            }

            default:
                neterr("type", cn < 0);
                return;
        }
        if(initmap)
        {
            int mode = gamemode;
            const char *map = getclientmap();
            if((multiplayer(false) && !m_mp(mode)) || (mode!=1 && !map[0]))
            {
                mode = remote ? lobbymode : localmode;
                map = remote ? lobbymap : localmap;
            }
            changemap(map, mode);
        }
    }

    void receivefile(uchar *data, int len)
    {
        ucharbuf p(data, len);
        int type = getint(p);
        data += p.length();
        len -= p.length();
        switch(type)
        {
            case SV_SENDDEMO:
            {
                defformatstring(fname)("%d.dmo", lastmillis);
                stream *demo = openrawfile(fname, "wb");
                if(!demo) return;
                conoutf("received demo \"%s\"", fname);
                demo->write(data, len);
                delete demo;
                break;
            }

            case SV_SENDMAP:
            {
                if(gamemode!=1) return;
                string oldname;
                copystring(oldname, getclientmap());
                defformatstring(mname)("getmap_%d", lastmillis);
                defformatstring(fname)("packages/base/%s.ogz", mname);
                stream *map = openrawfile(fname, "wb");
                if(!map) return;
                conoutf("received map");
                map->write(data, len);
                delete map;
                load_world(mname, oldname[0] ? oldname : NULL);
                remove(findfile(fname, "rb"));
                break;
            }
        }
    }

    void parsepacketclient(int chan, packetbuf &p)   // processes any updates from the server
    {
        switch(chan)
        {
            case 0:
                parsepositions(p);
                break;

            case 1:
                parsemessages(-1, NULL, p);
                break;

            case 2:
                receivefile(p.buf, p.maxlen);
                break;
        }
    }

    void getmap()
    {
        if(gamemode!=1) { conoutf(CON_ERROR, "\"getmap\" only works in coop edit mode"); return; }
        conoutf("getting map...");
        addmsg(SV_GETMAP, "r");
    }
    COMMAND(getmap, "");

    void stopdemo()
    {
        if(remote)
        {
            if(player1->privilege<PRIV_ADMIN) return;
            addmsg(SV_STOPDEMO, "r");
        }
        else server::stopdemo();
    }
    COMMAND(stopdemo, "");

    void recorddemo(int val)
    {
        if(remote && player1->privilege<PRIV_ADMIN) return;
        addmsg(SV_RECORDDEMO, "ri", val);
    }
    ICOMMAND(recorddemo, "i", (int *val), recorddemo(*val));

    void cleardemos(int val)
    {
        if(remote && player1->privilege<PRIV_ADMIN) return;
        addmsg(SV_CLEARDEMOS, "ri", val);
    }
    ICOMMAND(cleardemos, "i", (int *val), cleardemos(*val));

    void getdemo(int i)
    {
        if(i<=0) conoutf("getting demo...");
        else conoutf("getting demo %d...", i);
        addmsg(SV_GETDEMO, "ri", i);
    }
    ICOMMAND(getdemo, "i", (int *val), getdemo(*val));

    void listdemos()
    {
        conoutf("listing demos...");
        addmsg(SV_LISTDEMOS, "r");
    }
    COMMAND(listdemos, "");

    void sendmap()
    {
        if(gamemode!=1 || (player1->state==CS_SPECTATOR && remote && !player1->privilege)) { conoutf(CON_ERROR, "\"sendmap\" only works in coop edit mode"); return; }
        conoutf("sending map...");
        defformatstring(mname)("sendmap_%d", lastmillis);
        save_world(mname, true);
        defformatstring(fname)("packages/base/%s.ogz", mname);
        stream *map = openrawfile(fname, "rb");
        if(map)
        {
            int len = map->size();
            if(len > 1024*1024) conoutf(CON_ERROR, "map is too large");
            else if(len <= 0) conoutf(CON_ERROR, "could not read map");
            else sendfile(-1, 2, map);
            delete map;
        }
        else conoutf(CON_ERROR, "could not read map");
        remove(findfile(fname, "rb"));
    }
    COMMAND(sendmap, "");

    void gotoplayer(const char *arg)
    {
        if(player1->state!=CS_SPECTATOR && player1->state!=CS_EDITING) return;
        int i = parseplayer(arg);
        if(i>=0)
        {
            fpsent *d = getclient(i);
            if(!d || d==player1) return;
            player1->o = d->o;
            vec dir;
            vecfromyawpitch(player1->yaw, player1->pitch, 1, 0, dir);
            player1->o.add(dir.mul(-32));
            player1->resetinterp();
        }
    }
    COMMANDN(goto, gotoplayer, "s");

    void gotosel()
    {
        if(player1->state!=CS_EDITING) return;
        player1->o = getselpos();
        vec dir;
        vecfromyawpitch(player1->yaw, player1->pitch, 1, 0, dir);
        player1->o.add(dir.mul(-32));
        player1->resetinterp();
    }
    COMMAND(gotosel, "");
}

