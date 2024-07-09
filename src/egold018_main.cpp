#include "timerISR.h"
#include "helper.h"
#include "periph.h"
#include "serialATmega.h"
#include "math.h"
#include "spiAVR.h"
#include <stdlib.h>
#include "Bitmaps.h"

#define NUM_TASKS 3

#define SRCLK 3
#define RCLK 0
#define SER 2

#define BGR 13
#define BGG 16
#define BGB 33

#define prescaler8 0x02;
#define prescaler64 0x03;
#define prescaler256 0x04;
#define prescaler1024 0x05;

const char NUM_PROJECTILES = 20;
bool Q7 = false;

typedef struct _task{
	signed 	 char state;
	unsigned long period;
	unsigned long elapsedTime;
	int (*TickFct)(int);
} task;
typedef struct _buzzer{
  unsigned char pitch;
  int duration;
} Buzzer;
Buzzer buzzer = {0,0};
typedef struct _highscore{
  char world;
  char level;
} highscore;
highscore HighScore = {0, 0};

void ClearArea(char,char,char,char);

#pragma region Classes 
class Player {
  public:
    void Init();
    unsigned char position;
    int cooldown;
    int ultCd; // Max of 20 (20000)
    char health;
    unsigned char yPos = 110;
    void Step();
    void Render();
    bool active = false;
    bool render;
    bool powerup = false;
};

enum EnemyStates {DROP, HOVER, ATTACK};
class Enemy {
  public:
    Enemy();
    void Step();
    void Render(bool hit);
    bool active = false;
    float xPos;
    float yPos;
    char speed;
    unsigned char targetY;
    char size = 5; // size in each direction
    char health = 2;
    bool render = false;
    int delay;
    void Kill();
    int attackCooldown;
    char State;
    bool powerup;
  
  private:
    char type; // 0-2 for waypoints, 3 for parabola
    float xVelocity;
    float yVelocity;
};

class Projectile {
  public:
    void Step();
    void Render();
    bool owner = 0; // 1 for player owned, 0 for enemy owned
    bool active = false;
    float xVelocity;
    float yVelocity;
    float xPosition;
    float yPosition;
    bool big;
};

class Game {
  public:
    Game();
    void Start();
    bool playing = false;
    bool completed = false;
    bool gameOver = false;
    unsigned char world = 0;
    unsigned char level = 0;
    Player player;
    unsigned char numEnemies;
    unsigned char remainingEnemies;
    Projectile projectiles[NUM_PROJECTILES]; //array for holding projectiles
    Enemy enemies[];
    void Step();
};
Game game;

// Periods
const char REFRESH_RATE = 60;
const unsigned long DISPLAY_PERIOD = 1;
const unsigned long GAME_PERIOD = 1000 / REFRESH_RATE;
const unsigned long BUZZER_PERIOD = 50;
unsigned long GCD_PERIOD = 0;

task tasks[NUM_TASKS];

#pragma region Helpers
char RandomNumber(char lower_bound, char upper_bound) {
	return lower_bound + (rand() % (upper_bound - lower_bound + 1));
}
unsigned long findTasksGCD(task arr[], int n) {
  int result = arr[0].period;
  for (int i = 1; i < n; i++)
    result = findGCD(result, arr[i].period);
  return result;
}
uint16_t GetBit16(int x, int k) {
  return ((x & (0x01 << k)) != 0);
}
void shiftOut(char data) {
  for (int i = 0; i < 7; i++) {
    PORTB = SetBit(PORTB, SER, GetBit(data, i));
    PORTD = SetBit(PORTD, SRCLK, 1);
    _delay_us(1);
    PORTD = SetBit(PORTD, SRCLK, 0);
  }
  //PORTB = SetBit(PORTB, SER, Q7); // For ult led3

  PORTB = SetBit(PORTB, RCLK, 1);
  _delay_us(1);
  PORTB = SetBit(PORTB, RCLK, 0);
}
Projectile& getProjectile() {
  for (int i = 0; i < NUM_PROJECTILES; i++)
    if (!game.projectiles[i].active)
      return game.projectiles[i];
}
void Send_Command(char data) {
  PORTB = SetBit(PORTB, PIN_SS, 0);
  PORTD = SetBit(PORTD, 7, 0);
  SPI_SEND(data);
}
void Send_Data(char data) {
  PORTB = SetBit(PORTB, PIN_SS, 0);
  PORTD = SetBit(PORTD, 7, 1);
  SPI_SEND(data);
}
void ST7735_init() {
  // Reset
  PORTD = SetBit(PORTD, 2, 0);
  //shiftOut(0x00);
  _delay_ms(200);
  PORTD = SetBit(PORTD, 2, 1);
  //shiftOut(0x80);
  _delay_ms(200);

  // Init
  Send_Command(0x01); // SW RESET
  _delay_ms(150);
  Send_Command(0x11); // SLPOUT
  _delay_ms(200);
  Send_Command(0x0C); // COLMOD
  Send_Data(0x06); //for 18 bit color mode. You can pick any color mode you want
  _delay_ms(10);
  Send_Command(0x29); // DISPON
  _delay_ms(200);
}


#pragma region Class Functions
Game::Game() {
  Player player;
  player.position = 64;
  this->player = player;
}

void Game::Start() {
  numEnemies = world * 3 + level;
  remainingEnemies = numEnemies;
  for (int i = 0; i < numEnemies; i++) {
    Enemy newEnemy;
    newEnemy.delay = 4 * REFRESH_RATE * i;
    newEnemy.health = 1;
    newEnemy.active = true;
    newEnemy.xPos = RandomNumber(10, 117);
    newEnemy.yPos = 0;
    newEnemy.speed = RandomNumber(5, 10);
    newEnemy.targetY = RandomNumber(10, 70);
    newEnemy.State = DROP;
    newEnemy.attackCooldown = RandomNumber(4,16) * REFRESH_RATE;
    newEnemy.powerup = i == 0;

    //newEnemy.Render(false);
    this->enemies[i] = newEnemy;
  }

  player.health = 3;
  player.render = false;
  player.powerup = false;

  completed = false;
  playing = true;
  game.player.Render();
}

void Game::Step() {
  for (int i = 0; i < NUM_PROJECTILES; i++)
    projectiles[i].Step();
  for (int i = 0; i < numEnemies; i++)
    enemies[i].Step();

  player.Step();

  if (remainingEnemies <= 0) {
    completed = true;
    playing = false;
  } else if (player.health <= 0) {
    gameOver = true;
    playing = false;
  }
}

Enemy::Enemy() { // Randomize type
  type = 1;
  xVelocity = 0;
  yVelocity = 0;
  health = 2;
  active = false;
}

void Enemy::Step() {
  if (delay > 0) delay--;
  if (!active || delay > 0) return;

  switch (State) {
    case DROP:
      if (yPos >= targetY) {
        xVelocity = RandomNumber(1,2) == 1 ? speed/10.0 : -speed/10.0;
        yVelocity = 0;
        State = HOVER;
      } else {
        yVelocity = .3;
      }
      break;
    case HOVER:
      if (xVelocity > 0 && xPos >= 117) //turn left
        xVelocity = -speed/10.0;
      else if (xVelocity < 0 && xPos <= 10) //turn right
        xVelocity = speed/10.0;

      if (attackCooldown <= 0) {
        if (abs(xPos - game.player.position) < 3) {
          State = ATTACK;
          xVelocity = 0;
          yVelocity = .5;
        }
      } else {
        attackCooldown--;
      } 
      break;
    case ATTACK:
      if (yVelocity > 0 && yPos > 110) {
        yVelocity = -.3;
      } else if (yPos<= targetY) {
        xVelocity = RandomNumber(1,2) == 1 ? speed/10.0 : -speed/10.0;
        yVelocity = 0;
        State = HOVER;
        attackCooldown = RandomNumber(8, 16) * REFRESH_RATE;
      }
      break;
  }

  char oldx = xPos;
  char oldy = yPos;
  xPos += xVelocity;
  yPos += yVelocity;

  bool hit = false;
  for (int i = 0; i < NUM_PROJECTILES; i++) {
    Projectile& p = game.projectiles[i];
    
    if (p.active && p.owner == 1 && health > 0) {
      if (p.xPosition <= xPos + 4 && p.xPosition >= xPos - 5 && p.yPosition <= yPos + 4 && p.yPosition >= yPos - 4) {
        ClearArea(p.xPosition, (char)p.yPosition-1,2,4); //clear projectile
        hit = true;

        if (p.big) health -= 10;
        else health--;

        p.active = false;
        if (health <= 0) {
          buzzer.pitch = prescaler8;
          buzzer.duration = 200;
          break;
        }
      }
    }
  }

  if (health <= 0) {
    if (powerup)
      game.player.powerup = true;
    hit = true;
    active = false;
    game.remainingEnemies--;
  }

  if (hit) { // render as white or clear if head
    ClearArea((char)xPos - 5, (char)yPos - 4, 9, 10);
    if (health > 0) {
      Render(true);
      render = true;
    }
  } else if (oldx != (char)xPos || oldy != (char)yPos || render) { // always render
    ClearArea((char)xPos - 5, (char)yPos - 4, 9, 10);
    Render(false);
    render = false;
  }
}

void Player::Init() {
  active = true;
  cooldown = 0;
  ultCd = 24 * REFRESH_RATE;
  position = 64;
  health = 3;
  Render();
  PORTB = SetBit(PORTB, 4, 0);
  PORTD = SetBit(PORTD, 4, 0);
}

void Player::Step() {
  if (cooldown > 0)
    cooldown--;
  if (ultCd > 0)
    ultCd--;
  
  if (ADC_read(2) < 500 && ultCd <= 0) { // Charged attack
    cooldown = (1.5) * REFRESH_RATE;
    ultCd = (20) * REFRESH_RATE;
    Projectile& shot = getProjectile();
    shot.xPosition = game.player.position;
    shot.yPosition = game.player.yPos - 10;
    shot.xVelocity = 0;
    shot.yVelocity = -10;
    shot.owner = 1;
    shot.big = true;
    shot.active = true;
    buzzer.pitch = prescaler256;
    buzzer.duration = 300;
  } else if (ADC_read(5) > 500 && cooldown <= 0) { // Shoot
    Projectile& shot = getProjectile();
    shot.xPosition = game.player.position;
    shot.yPosition = game.player.yPos - 10;
    shot.xVelocity = 0;
    shot.yVelocity = -10;
    shot.owner = 1;
    shot.big = false;
    shot.active = true;

    if (powerup) {
      Projectile& shotL = getProjectile();
      shotL.xPosition = game.player.position;
      shotL.yPosition = game.player.yPos - 10;
      shotL.xVelocity = -15;
      shotL.yVelocity = -10;
      shotL.owner = 1;
      shotL.big = false;
      shotL.active = true;

      Projectile& shotR = getProjectile();
      shotR.xPosition = game.player.position;
      shotR.yPosition = game.player.yPos - 10;
      shotR.xVelocity = 15;
      shotR.yVelocity = -10;
      shotR.owner = 1;
      shotR.big = false;
      shotR.active = true;
    }

    cooldown = 0.5 * REFRESH_RATE;
    buzzer.pitch = prescaler64;
    buzzer.duration = 100;
  }

  bool hit = false;
  for (int i = 0; i < game.numEnemies; i++) {
    Enemy& p = game.enemies[i];
    if (p.active) {
      if (abs(p.xPos - position) < 8 && abs(p.yPos - yPos) < 10) {
        health--;
        hit = true;
        p.health = 0;
        
        buzzer.pitch = prescaler1024;
        buzzer.duration = 200;

        break;
      }
    }
  }

  if (ADC_read(1) > 600) {
    if (position < 120) {
      //ClearArea(position - 8, 16, yPos - 9, 17);
      position++;
    }
  } else if (ADC_read(1) < 400) {
    if (position > 8) {
      //ClearArea(position - 8, 16, yPos - 9, 17);
      position--;
    }
  } else if (hit) {
  }
  Render();

  PORTB = SetBit(PORTB, 4, game.playing && ultCd <= 0);
  PORTD = SetBit(PORTD, 4, game.playing && ultCd <= (10 * REFRESH_RATE));
}

void Projectile::Step() {
  if (!active) return;
  ClearArea(xPosition-1, (char)yPosition-1,4,6);
  if (yPosition <= 0) active = false;
  xPosition += xVelocity / REFRESH_RATE;
  yPosition -= 3;// += yVelocity / REFRESH_RATE;
  Render();
}

#pragma region Screen Functions

void Player::Render() {
  Send_Command(0x2A);
  Send_Data(0x00);
  Send_Data(position - 8);
  Send_Data(0x00);
  Send_Data(position + 8);

  Send_Command(0x2B);
  Send_Data(0x00);
  Send_Data(yPos - 8); // x
  Send_Data(0x00);
  Send_Data(yPos + 9); // x+8

  Send_Command(0x2C);
  for (int i = 0; i < 17; i++) {
    for (int j = 16; j >= 0; j--) {
      if (GetBit16(PlayerMap[i], j)) {
        if (!game.playing) {
          Send_Data(0x90);
          Send_Data(0x90);
          Send_Data(0x90);
        } else {
          Send_Data(health == 3 ? 0xFF : health == 2 ? 0x4F : 0x00); //B
          Send_Data(health == 3 ? 0xFF : health == 2 ? 0xD9 : 0x00); //G
          Send_Data(health == 3 ? 0xFF : health == 2 ? 0xFF : 0xFF); //R
        }
      } else {
        Send_Data(BGB);
        Send_Data(BGG);
        Send_Data(BGR);
      }
    }
  }
}
void Projectile::Render() {
  Send_Command(0x2A);
  Send_Data(0x00);
  Send_Data(xPosition);
  Send_Data(0x00);
  Send_Data(xPosition + 1);

  Send_Command(0x2B);
  Send_Data(0x00);
  Send_Data((char)yPosition - 2); // x
  Send_Data(0x00);
  Send_Data((char)yPosition + 2); // x+8

  Send_Command(0x2C);
  for (int i = 0; i < 8; i++) {
    if (owner == 1) { //green
      Send_Data(big ? 0x18 : 0x00);
      Send_Data(big ? 0xbe : 0xff);
      Send_Data(big ? 0xfe : 0x00);
    } else { //red
      Send_Data(0x00);
      Send_Data(0x00);
      Send_Data(0xff);
    }
  }
}
void Enemy::Render(bool hit) {
  Send_Command(0x2A);
  Send_Data(0x00);
  Send_Data(xPos-4);
  Send_Data(0x00);
  Send_Data(xPos + 3);

  Send_Command(0x2B);
  Send_Data(0x00);
  Send_Data(yPos-4); // x
  Send_Data(0x00);
  Send_Data(yPos+3); // x+8

  Send_Command(0x2C);
  for (int i = 0; i < 8; i++) {
    for (int j = 7; j >= 0; j--) {
      if (GetBit(RammerMap[i], j)) {
        Send_Data(hit ? 0xFF : powerup ? 0x16 : 0x6E);
        Send_Data(hit ? 0xFF : powerup ? 0x9C : 0x6E);
        Send_Data(hit ? 0xFF : powerup ? 0xF5 : 0xEF);
      } else {
        Send_Data(BGB);
        Send_Data(BGG);
        Send_Data(BGR);
      }
    }
  }
}

// SCREEN FUNCTIONS
void ClearScreen() {
  Send_Command(0x2A);
  Send_Data(0x00);
  Send_Data(0x00); //48
  Send_Data(0x00);
  Send_Data(0xFF); //80

  Send_Command(0x2B);
  Send_Data(0x00);
  Send_Data(0x00); //48
  Send_Data(0x00);
  Send_Data(0xFF); //80

  Send_Command(0x2C);
  for (unsigned int i = 0; i < 50000; i++) { // for each pixel?
    Send_Data(BGB);
    Send_Data(BGG);
    Send_Data(BGR);
  }
}

void Write8x10(char x, char y, const uint8_t map[10]) {
  //serial_println("here");
  Send_Command(0x2A);
  Send_Data(0x00);
  Send_Data(x); // x
  Send_Data(0x00);
  Send_Data(x+7); // x+8

  Send_Command(0x2B);
  Send_Data(0x00);
  Send_Data(y); // x
  Send_Data(0x00);
  Send_Data(y+9); // x+8

  Send_Command(0x2C);
  for (int i = 0; i < 10; i++) {
    for (int j = 7; j >= 0; j--) {
      if (GetBit(map[i], j)) {
        Send_Data(0xff);
        Send_Data(0xff);
        Send_Data(0xff);
      } else {
        Send_Data(BGB);
        Send_Data(BGG);
        Send_Data(BGR);
      }
    }
  }
}

void Write8x8(char x, char y, const uint8_t map[8]) {
  //serial_println("here");
  Send_Command(0x2A);
  Send_Data(0x00);
  Send_Data(x); // x
  Send_Data(0x00);
  Send_Data(x+7); // x+8

  Send_Command(0x2B);
  Send_Data(0x00);
  Send_Data(y); // x
  Send_Data(0x00);
  Send_Data(y+7); // x+8

  Send_Command(0x2C);
  for (int i = 0; i < 8; i++) {
    for (int j = 7; j >= 0; j--) {
      if (GetBit(map[i], j)) {
        Send_Data(0xff);
        Send_Data(0xff);
        Send_Data(0xff);
      } else {
        Send_Data(BGB);
        Send_Data(BGG);
        Send_Data(BGR);
      }
    }
  }
}

void Write4x5(char x, char y, const uint8_t map[10]) {
  //serial_println("here");
  Send_Command(0x2A);
  Send_Data(0x00);
  Send_Data(x);
  Send_Data(0x00);
  Send_Data(x+3);

  Send_Command(0x2B);
  Send_Data(0x00);
  Send_Data(y);
  Send_Data(0x00);
  Send_Data(y+4);

  Send_Command(0x2C);
  for (int i = 0; i < 10; i+=2) {
    for (int j = 7; j >= 0; j-=2) {
      if (GetBit(map[i], j)) {
        Send_Data(0xff);
        Send_Data(0xff);
        Send_Data(0xff);
      } else {
        Send_Data(BGB);
        Send_Data(BGG);
        Send_Data(BGR);
      }
    }
  }
}

void ClearArea(char x, char y, char w, char h) {
  //serial_println("here");
  Send_Command(0x2A);
  Send_Data(0x00);
  Send_Data(x); // x
  Send_Data(0x00);
  Send_Data(x+w); // x+8

  Send_Command(0x2B);
  Send_Data(0x00);
  Send_Data(y); // x
  Send_Data(0x00);
  Send_Data(y+h); // x+8

  Send_Command(0x2C);
  for (int i = 0; i < w * h; i++) {
    Send_Data(BGB);
    Send_Data(BGG);
    Send_Data(BGR);
  }
}
// Tick Functions
#pragma region Tick Functions

enum gameStates {WAITING, PLAYING, WIN, LOSE};
int GameTick(int state) {
  static unsigned int ct = 0;
  switch(state) { // State Transitions
    case WAITING: // Is only here when game is not started
      if (ADC_read(0) > 500) {
        serial_println("Game Starting");
        ClearArea(0, 0, 127, 70); //Clear Press Start
        state = PLAYING;
        game.world = 1;
        game.level = 1;
      }
      break;
    case PLAYING:
      if (game.completed) {
        ct = 0;
        game.player.Render();
        state = WIN;
        game.level++;
        if (game.level >= 4) {
          game.level = 1;
          game.world++;
        }
      } else if (game.gameOver) {
        ct = 0;
        state = LOSE;
      }
      break;
    case WIN:
      if (ct == 0) {
        Write8x10(48, 30, alphabet['w'-'a']);
        Write8x10(60, 30, alphabet['i'-'a']);
        Write8x10(72, 30, alphabet['n'-'a']);
      } else if (ct == 6 * REFRESH_RATE) {
        Write8x10(48, 30, numbers[game.world]);
        Write8x10(60, 30, dashbm);
        Write8x10(72, 30, numbers[game.level]);
      } else if (ct == 5 * REFRESH_RATE || ct == 11 * REFRESH_RATE) {
        ClearArea(36, 30, 91, 25);
      }

      ct++;
      if (ct > 11 * REFRESH_RATE)
        state = PLAYING;
      break;
    case LOSE:
      if (ct == 0) {
        ClearScreen();
        Write8x10(48-6, 30, alphabet['l'-'a']);
        Write8x10(60-6, 30, alphabet['o'-'a']);
        Write8x10(72-6, 30, alphabet['s'-'a']);
        Write8x10(84-6, 30, alphabet['e'-'a']);
      }

      ct++;
      if (ct >= 5 * REFRESH_RATE) {
        ClearArea(36, 30, 91, 25);
        game = Game();
        game.player.Init();
        state = WAITING;
      }
      break;
  }
  
  switch(state) { // State Actions
    case WAITING:    
      game.level = 1;
      game.world = 1;
  
      if (ct == 0) {
        ct = 2 * REFRESH_RATE;
        Write8x10(36, 30, alphabet['p'-'a']);
        Write8x10(48, 30, alphabet['r'-'a']);
        Write8x10(60, 30, alphabet['e'-'a']);
        Write8x10(72, 30, alphabet['s'-'a']);
        Write8x10(84, 30, alphabet['s'-'a']);

        Write8x10(36, 45, alphabet['s'-'a']);
        Write8x10(48, 45, alphabet['t'-'a']);
        Write8x10(60, 45, alphabet['a'-'a']);
        Write8x10(72, 45, alphabet['r'-'a']);
        Write8x10(84, 45, alphabet['t'-'a']);

        Write4x5(3, 1, alphabet['h'-'a']);
        Write4x5(9, 1, alphabet['i'-'a']);

        Write4x5(18, 1, numbers[HighScore.world]);
        Write4x5(24, 1, dashbm);
        Write4x5(30, 1, numbers[HighScore.world]);
      } else if (ct == REFRESH_RATE) {
        ClearArea(36, 30, 91, 25);
      }
      ct--;
      
      break;
    case PLAYING:
      if (!game.playing) // Initialize level
        game.Start();
      game.Step();
      break;
    case WIN:
      if (game.world > HighScore.world) {
        HighScore.world = game.world;
        HighScore.level = game.level;
      } else if (game.world == HighScore.world && game.level > HighScore.level) {
        HighScore.level = game.level;
      }
      break;
  }
  
  return state;
}

enum displayStates {ONE, TWO, THREE};
int DisplayTick(int state) {
  switch(state) { // State Transitions
    case ONE:
      state = TWO;
      break;
    case TWO:
      state = THREE;
      break;
    case THREE:
      state = ONE;
      break;
  }
  
  shiftOut(0x00);
  PORTC = 0xFF;
  PORTD = SetBit(PORTD, 5, 1);
  switch(state) { // State Actions
    case ONE:
      shiftOut(nums[game.world]);
      PORTC = SetBit(PORTC, 3, 0);
      break;
    case TWO:
      shiftOut(dash);
      PORTC = SetBit(PORTC, 4, 0);
      break;
    case THREE:
      shiftOut(nums[game.level]);
      PORTD = SetBit(PORTD, 5, 0);
      break;
  }

  return state;
}

enum BuzzerStates {BUZZER_ACTIVE};
int BuzzerTick(int state) {
  if (buzzer.duration > 0) {
    OCR0A = 128;
    buzzer.duration -= BUZZER_PERIOD;
    TCCR0B = (TCCR0B & 0xF8) | buzzer.pitch;
  } else {
    OCR0A = 255;
  }
  return state;
}


#pragma region Main

void TimerISR() {
    
    //TODO: sample inputs here

	for ( unsigned int i = 0; i < NUM_TASKS; i++ ) {                   // Iterate through each task in the task array
		if ( tasks[i].elapsedTime == tasks[i].period ) {           // Check if the task is ready to tick
			tasks[i].state = tasks[i].TickFct(tasks[i].state); // Tick and set the next state for this task
			tasks[i].elapsedTime = 0;                          // Reset the elapsed time for the next tick
		}
		tasks[i].elapsedTime += GCD_PERIOD;                        // Increment the elapsed time by GCD_PERIOD
	}
}


int main(void) {
  srand(123);

  _delay_ms(1000);
  //TODO: initialize all your inputs and ouputs
  DDRB = 0xFF;
  DDRD = 0xFF;
  DDRC = 0x18; // 011000

  ADC_init();   // initializes ADC
  serial_init(9600);
  SPI_INIT();
  ST7735_init();

  OCR0A = 128; // sets duty cycle to 50% since TOP is always 256
  
  TCCR0A |= (1 << COM0A1);               // use Channel A
  TCCR0A |= (1 << WGM01) | (1 << WGM00); // set fast PWM Mode

  tasks[0] = {0, GAME_PERIOD, 0, &GameTick};
  tasks[1] = {0, DISPLAY_PERIOD, 0, &DisplayTick}; // Display
  tasks[2] = {0, BUZZER_PERIOD, 0, &BuzzerTick};

  PORTB = 0x00;
  PORTD = 0x04;
  PORTC = 0xFF;

  ClearScreen();
  game.player.Init();

  GCD_PERIOD = findTasksGCD(tasks, NUM_TASKS);
  TimerSet(GCD_PERIOD);
  TimerOn();

  while (1) {}

  return 0;
}