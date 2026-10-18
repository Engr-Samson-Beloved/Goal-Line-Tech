#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <esp_arduino_version.h>

constexpr uint8_t SDA_PIN=4,SCL_PIN=5,LED_PIN=3,CHANNEL=1;
const char *SSID="GOAL-LINE-REFEREE",*PASSWORD="GoalLine2026";
uint8_t postMac[]={0xF4,0x2D,0xC9,0x70,0x70,0xB0};
enum Type:uint8_t{PKT_STATUS=1,COMMAND=2,ACK=3};
enum State:uint8_t{READY,ACTIVE,PAUSED,STOPPED};
enum Cmd:uint8_t{START=1,PAUSE,RESUME,RESET,CLEAR,END};
struct __attribute__((packed)) Packet{uint8_t type,command,mask,count;uint32_t sequence,timestamp;};
WebServer server(80);
uint8_t oledAddress=0x3C; bool oledPresent=false;
const uint8_t font[][5]={{0x3E,0x51,0x49,0x45,0x3E},{0,0x42,0x7F,0x40,0},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{1,0x71,9,5,3},{0x36,0x49,0x49,0x49,0x36},{6,0x49,0x49,0x29,0x1E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},{0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,9,9,9,1},{0x3E,0x41,0x49,0x49,0x7A},{0x7F,8,8,8,0x7F},{0,0x41,0x7F,0x41,0},{0x20,0x40,0x41,0x3F,1},{0x7F,8,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},{0x7F,2,0x0C,2,0x7F},{0x7F,4,8,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},{0x7F,9,9,9,6},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,9,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},{1,1,0x7F,1,1},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,8,0x14,0x63},{7,8,0x70,8,7},{0x61,0x51,0x49,0x45,0x43}};
Packet inbox={}; volatile bool pending=false;
State state=READY; bool alert=false; uint8_t alertCount=0;
uint32_t elapsed=0,started=0,lastPost=0,lastSequence=0,goals=0,varChecks=0,lastStateSync=0;

uint32_t matchMs(){return elapsed+(state==ACTIVE?millis()-started:0);}
void clockText(char *s){uint32_t t=matchMs()/1000;sprintf(s,"%02lu:%02lu",(unsigned long)(t/60),(unsigned long)(t%60));}
uint8_t glyph(char c){if(c>='0'&&c<='9')return c-'0';if(c>='A'&&c<='Z')return 10+c-'A';if(c==':')return 36;return 255;}
void command(uint8_t c){Wire.beginTransmission(oledAddress);Wire.write(0);Wire.write(c);Wire.endTransmission();}
void oledInit(){const uint8_t s[]={0xAE,0xD5,0x80,0xA8,0x3F,0xD3,0,0x40,0xAD,0x8B,0xA1,0xC8,0xDA,0x12,0x81,0x7F,0xD9,0x22,0xDB,0x35,0xA4,0xA6,0xAF};for(uint8_t c:s)command(c);}
uint8_t frame[1024]={};
void pixel(uint8_t x,uint8_t y){if(x<128&&y<64)frame[(y/8)*128+x]|=1u<<(y%8);}
void largeText(const char*s,uint8_t y,uint8_t scale){
  size_t n=strlen(s);int x=(128-(int(n)*6-1)*scale)/2;
  for(size_t i=0;i<n;i++){
    uint8_t g=glyph(s[i]);
    if(g!=255)for(uint8_t col=0;col<5;col++)for(uint8_t row=0;row<7;row++){
      uint8_t bits=g==36?(col==1||col==2?0x36:0):font[g][col];
      if(bits&(1u<<row))for(uint8_t dx=0;dx<scale;dx++)for(uint8_t dy=0;dy<scale;dy++)pixel(x+col*scale+dx,y+row*scale+dy);
    }
    x+=6*scale;
  }
}
void showFrame(){
  static uint8_t sent[1024]={};static bool first=true;
  for(uint8_t page=0;page<8;page++)for(uint8_t block=0;block<8;block++){
    uint16_t at=page*128+block*16;
    if(!first&&memcmp(frame+at,sent+at,16)==0)continue;
    uint8_t column=2+block*16;
    Wire.beginTransmission(oledAddress);Wire.write(0);Wire.write(0xB0|page);Wire.write(column&15);Wire.write(0x10|(column>>4));Wire.endTransmission();
    Wire.beginTransmission(oledAddress);Wire.write(0x40);Wire.write(frame+at,16);Wire.endTransmission();
    memcpy(sent+at,frame+at,16);
  }
  first=false;
}
void draw(){
  static uint32_t last=0;if(!oledPresent||millis()-last<100)return;last=millis();
  memset(frame,0,sizeof(frame));char timer[12];clockText(timer);
  if(alert){
    if(alertCount==1){largeText("GOAL",8,4);largeText(timer,45,2);}
    else{largeText("CHECK",5,3);largeText("VAR",33,4);}
  }else{
    largeText("GOAL LINE",1,2);largeText(timer,20,3);
    const char*label=state==ACTIVE?"PLAY":state==PAUSED?"PAUSED":state==STOPPED?"ENDED":"READY";
    if(state!=PAUSED||(millis()/500)%2==0)largeText(label,47,2);
  }
  showFrame();
}
void addPeer(){if(esp_now_is_peer_exist(postMac))return;esp_now_peer_info_t p={};memcpy(p.peer_addr,postMac,6);p.channel=CHANNEL;esp_now_add_peer(&p);}
void sendCmd(Cmd c){addPeer();Packet p={};p.type=COMMAND;p.command=c;p.timestamp=millis();esp_now_send(postMac,(uint8_t*)&p,sizeof(p));}
void syncPostState(){Cmd c=state==ACTIVE?RESUME:state==PAUSED?PAUSE:state==STOPPED?END:RESET;sendCmd(c);}
void apply(Cmd c){if(c==START){elapsed=0;started=millis();state=ACTIVE;alert=false;}else if(c==PAUSE&&state==ACTIVE){elapsed=matchMs();state=PAUSED;}else if(c==RESUME&&state==PAUSED){started=millis();state=ACTIVE;}else if(c==RESET){elapsed=0;started=millis();state=READY;alert=false;goals=varChecks=0;}else if(c==CLEAR)alert=false;else if(c==END){if(state==ACTIVE)elapsed=matchMs();state=STOPPED;alert=false;}sendCmd(c);}
void receivePacket(const uint8_t*sender,const uint8_t*d,int n){if(n!=(int)sizeof(Packet)||memcmp(sender,postMac,6)!=0)return;Packet p;memcpy(&p,d,n);if(p.type!=PKT_STATUS)return;inbox=p;pending=true;Packet a={};a.type=ACK;a.sequence=p.sequence;esp_now_send(sender,(uint8_t*)&a,sizeof(a));}
#if ESP_ARDUINO_VERSION_MAJOR >= 3
void receive(const esp_now_recv_info*i,const uint8_t*d,int n){receivePacket(i->src_addr,d,n);}
#else
void receive(const uint8_t*sender,const uint8_t*d,int n){receivePacket(sender,d,n);}
#endif
String stateName(){return state==ACTIVE?"ACTIVE":state==PAUSED?"PAUSED":state==STOPPED?"STOPPED":"READY";}
String json(){char t[12];clockText(t);String s="{\"state\":\""+stateName()+"\",\"time\":\""+String(t)+"\",\"alert\":\""+String(alert?(alertCount==1?"GOAL":"CHECK VAR"):"")+"\"";s+=",\"blocked\":"+String(alert?alertCount:0)+",\"postOnline\":"+String(lastPost&&millis()-lastPost<3500?"true":"false")+",\"goals\":"+String(goals)+",\"varChecks\":"+String(varChecks)+"}";return s;}
void action(){String a=server.arg("do");if(a=="start")apply(START);else if(a=="pause")apply(PAUSE);else if(a=="resume")apply(RESUME);else if(a=="reset")apply(RESET);else if(a=="clear")apply(CLEAR);else if(a=="end")apply(END);else if(a=="goal"){alert=true;alertCount=1;goals++;}else if(a=="var"){alert=true;alertCount=2;varChecks++;}server.send(200,"application/json",json());}
void root(){const char page[] PROGMEM=R"HTML(<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1"><title>Goal Line Referee</title><style>body{font-family:Arial;background:#07130b;color:#fff;padding:16px}.wrap{max-width:700px;margin:auto}.card{background:#132219;padding:16px;border-radius:14px;margin:12px 0}h1{color:#64dd72}.time{font-size:52px;font-weight:bold}.alert{font-size:34px;color:#ffd740}.grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}button{padding:15px;border:0;border-radius:10px;font-weight:bold;font-size:16px;background:#42b952;color:white}.danger{background:#d84343}.muted{color:#b7c7bb}img{width:100%;background:#000;border-radius:10px}</style></head><body><div class=wrap><h1>Goal Line Referee</h1><div class=card><div id=s>READY</div><div class=time id=t>00:00</div><div class=alert id=a></div><div class=muted id=l>Goal post waiting...</div></div><div class="card grid"><button onclick="go('start')">Start Match</button><button onclick="go('pause')">Pause</button><button onclick="go('resume')">Resume</button><button onclick="go('clear')">Clear Alert</button><button onclick="go('goal')">Manual Goal</button><button onclick="go('var')">Check VAR</button><button class=danger onclick="go('reset')">Reset</button><button class=danger onclick="go('end')">End Match</button></div><div class=card><h2>Goal Post Camera</h2><img src="http://192.168.4.2:81/stream"><p class=muted>Camera address: 192.168.4.2</p></div></div><script>async function go(x){await fetch('/action?do='+x,{method:'POST'});up()}async function up(){let x=await(await fetch('/status')).json();s.textContent=x.state;t.textContent=x.time;a.textContent=x.alert;l.textContent=x.postOnline?'Goal post connected':'Goal post offline'}setInterval(up,500);up()</script></body></html>)HTML";server.send_P(200,"text/html",page);}
void setup(){Serial.begin(115200);delay(300);Serial.println("Wrist unit starting");pinMode(LED_PIN,OUTPUT);Wire.begin(SDA_PIN,SCL_PIN);Wire.setClock(100000);oledPresent=false;for(uint8_t a=0x3C;a<=0x3D;a++){Wire.beginTransmission(a);if(Wire.endTransmission()==0){oledAddress=a;oledPresent=true;break;}}if(oledPresent){oledInit();draw();Serial.printf("OLED found at 0x%02X\n",oledAddress);}else Serial.println("OLED not found at 0x3C or 0x3D");WiFi.mode(WIFI_AP_STA);bool apReady=WiFi.softAP(SSID,PASSWORD,CHANNEL,false,4);esp_wifi_set_channel(CHANNEL,WIFI_SECOND_CHAN_NONE);Serial.printf("Wrist STA MAC: %s; AP: %s\n",WiFi.macAddress().c_str(),apReady?"ready":"FAILED");if(esp_now_init()==ESP_OK){esp_now_register_recv_cb(receive);addPeer();Serial.println("ESP-NOW ready");}else Serial.println("ESP-NOW FAILED");server.on("/",HTTP_GET,root);server.on("/status",HTTP_GET,[]{server.send(200,"application/json",json());});server.on("/action",HTTP_POST,action);server.begin();Serial.printf("Phone: %s / %s / http://192.168.4.1\n",SSID,PASSWORD);}
void loop(){server.handleClient();if(pending){Packet p=inbox;pending=false;lastPost=millis();if(state==ACTIVE&&p.count&&p.sequence!=lastSequence){lastSequence=p.sequence;alert=true;alertCount=p.count;if(p.count==1)goals++;else varChecks++;}}if(millis()-lastStateSync>=2000){lastStateSync=millis();syncPostState();}digitalWrite(LED_PIN,alert);draw();delay(2);}
