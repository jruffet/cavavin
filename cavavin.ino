#include <OneWire.h>
#include <LiquidCrystal.h>
#include <PID_v1.h>
#include <avr/eeprom.h>

#define DS18S20_ID 0x10
#define DS18B20_ID 0x28
// Used to know if we have proper data saved in EEPROM
#define MAGIC_SAVE_NUMBER 0xdeadbeef
#define DISPLAYS 3
#define DIMDISPLAYTIME 30000
#define DIMDISPLAYPOWER 5 


#define BACKLIGHT 5
LiquidCrystal lcd(2, 3, 4, 6, 7, 8);

/* transistors / MOSFETS */
#define BACKFAN 9
#define FRONTFAN 10
#define PELTIER 11

/* push buttons */
#define PBN0 14 // (A0)
#define PBN1 15 // (A1)
#define PBN2 16 // (A2)
#define PBN3 17 // (A3)

OneWire onewireCave(18); // (A4)
OneWire onewireBack(19); // (A5)


struct menu {
	char title[17];
	double *value;
	double min;
	double max;
	double step;
	menu *next;
};

byte DSCave[8];
byte DSBack[8];
long lastAskDSCave = 0;
long lastAskDSBack = 0;


int curDisplay = 0;
bool dimDisplay = 0;
bool menuMode = 0;

double powerBackFan   = 100;
double powerFrontFan  = 255;
double powerPeltier   = 200;
double powerBackLight = 100;

double tempCave     = 0;
double tempBack     = 0;
double targetTemp   = 15;


long btnHold       = 0;
long btnLastApply  = 0;

menu *curMenu  = NULL;
menu *lastMenu = NULL;
	
double kp=30, ki=5, kd=0;
PID myPID(&tempCave, &powerPeltier, &targetTemp, kp,ki,kd, DIRECT);

/**********  TEMPERATURE FUNCTIONS  **********/
boolean findDS(OneWire *ow, byte *addr) {
    //find a device
    if (!ow->search(addr)) {
        ow->reset_search();
        return false;
    }
    if (OneWire::crc8( addr, 7) != addr[7]) {
        return false;
    }
    if (addr[0] != DS18S20_ID && addr[0] != DS18B20_ID) {
        return false;
    }
    ow->reset();
    return true;
}

void askTempDS(OneWire *ow, byte *addr) {
	ow->reset();
	ow->select(addr);
	ow->write(0x44, 1);
}

double readTempDS(OneWire *ow, byte *addr) {
	byte i;
	byte data[12];

	ow->reset();
	ow->select(addr);
	ow->write(0xBE);
	for ( i = 0; i < 9; i++) {
		data[i] = ow->read();
	}
	return ((double)(data[1] << 8) + data[0] ) / 16;
}

void getTempDS(OneWire *ow, byte *addr, double *temp, long *lastAsk) {
	double tempread;

	if (!*lastAsk) {
		*lastAsk = millis();
		askTempDS(ow, addr);
	// wait before reading
	} else if (abs((millis() - *lastAsk)) > 750) {
		*lastAsk = 0;
		tempread = readTempDS(ow, addr);
		// don't send incorrect temp (i.e. due to interferences)
		if (tempread > 1 && tempread < 70)
			*temp = tempread;
	}
}


/**********  LCD FUNCTIONS  **********/
void blankLcdLine(int line) {
	printLcdLine(line, "");
}

void blankLcdDisplay() {
	blankLcdLine(0);
	blankLcdLine(1);
}

void printLcd(int pos, int line, char *input) {
	lcd.setCursor(pos, line);
	lcd.print(input);
}

void printLcdLine(int line, char *input) {
	char text[17] = "                ";
	strncpy(text, input, strlen(input));
	printLcd(0, line, text);
}

void blinkLcdDisplay(int i) {
	for (; i > 0; i--) {
		delay(500);
		lcd.noDisplay();
		delay(500);
		lcd.display();
	}
}

void displayLines(char *text1, char *text2) {
	printLcdLine(0, text1);
	printLcdLine(1, text2);
	Serial.println(text1);
	Serial.println(text2);
}


/**********  DISPLAY FUNCTIONS  **********/
void displayTempTarget() {
	char text1[17], text2[17];
	snprintf(text1, 17, "Current   Target");
	snprintf(text2, 17, "%02d.%02d%cC    %d%cC", (int)tempCave, deci2(tempCave), (char)223, (int)targetTemp, (char)223);
	displayLines(text1, text2);
}

void displayTempPower() {
	char text1[17], text2[17];
	snprintf(text1, 17, "Tc=%02d.%02d TEC=%-03d", (int)tempCave, deci2(tempCave), (int)powerPeltier);
	snprintf(text2, 17, "Tb=%02d.%02d  BF=%-03d", (int)tempBack, deci2(tempBack), (int)powerBackFan);
	displayLines(text1, text2);
}

void displayPID() {
	char text1[17], text2[17];
	snprintf(text1, 17, "P=%02d.%02d I=%02d.%02d", (int)kp, deci2(kp), (int)ki, deci2(ki));
	snprintf(text2, 17, "D=%02d.%02d", (int)kd, deci2(kd));
	displayLines(text1, text2);
}

void cycleDisplay() {
	curDisplay++;
	curDisplay %= DISPLAYS;
}

void displayInfo() {
	if (curDisplay == 0) {
		displayTempTarget();
	} else if (curDisplay == 1) {
		displayTempPower();
	} else if (curDisplay == 2) {
		displayPID();
	}
}


/**********  MENU FUNCTIONS  **********/
void addMenuEntry(char *title, double *value, double min, double max, double step) {
	menu *temp;

	temp = (menu*)malloc(sizeof(menu));
	
	strncpy(temp->title, title, 16);
	temp->title[16] = 0;
	temp->value = value;
	temp->min = min;
	temp->max = max;
	temp->step = step;
	// first entry in the list
	if (curMenu == NULL) {
		temp->next = temp;
		lastMenu = temp;
	// tail points to last created entry
	} else {
		temp->next = curMenu;
		lastMenu->next = temp;
	}
	curMenu = temp;
}

void cycleMenus() {
	curMenu = curMenu->next;
}

void displayMenu() {
	char text[17];
	if ((int)curMenu->step == curMenu->step)
		snprintf(text, 17, "value : %d", (int)*curMenu->value);
	else
		snprintf(text, 17, "value : %02d.%02d", (int)*curMenu->value, deci2(*curMenu->value));
	displayLines(curMenu->title, text);
}

void initMenus() {
	addMenuEntry("PID kd", &kd, 0, 100, .5);
	addMenuEntry("PID ki", &ki, 0, 100, .5);
	addMenuEntry("PID kp", &kp, 0, 100, .5);
	addMenuEntry("backlight", &powerBackLight, 0, 255, 5);
	addMenuEntry("backfan speed", &powerBackFan, 0, 255, 10);
	addMenuEntry("target temp", &targetTemp, 0, 100, 1);
}
	

/**********  INPUT FUNCTIONS  **********/
void processSerialCommand() {
	char command;
	char input[256];
	int arg;

	// input : b55* -> backfan to 55
	memset(input, 0, sizeof(input));
	Serial.readBytesUntil('*', input, 255);

	command = input[0];
	sscanf(input + 1, "%d", &arg);
	if (arg > 255)
		arg = 255;
	else if (arg < 0)
		arg =0;

	if (command == 'p') { 
		powerPeltier = arg; 
	} else if (command == 'b') {
		powerBackFan = arg;
	} else if (command == 'f') { 
		powerFrontFan = arg;
	} else if (command == 'l') { 
		powerBackLight = arg;
	} else if (command == '0') {
		TCCR0B = TCCR1B & 0b11111000 | arg;
	} else if (command == '1') {
		TCCR1B = TCCR1B & 0b11111000 | arg;
	} else if (command == '2') {
		TCCR2B = TCCR1B & 0b11111000 | arg;
	} else if (command == 't') {
		targetTemp = arg;
	} else if (command == 'd') {
		cycleDisplay();
	} else if (command == 's') {
		saveSettings();
	} else if (command == 'm') {
		myPID.SetMode(MANUAL);
	} else if (command == 'a') {
		myPID.SetMode(AUTOMATIC);
	}

	setValues();
}

void processPushBtnInput() {
	bool btnPushed = 0;

	if (   digitalRead(PBN0) == HIGH
		|| digitalRead(PBN1) == HIGH
		|| digitalRead(PBN2) == HIGH
		|| digitalRead(PBN3) == HIGH ) 
	{
		// wait before repeating when holding a button
		if (!btnHold || abs((millis() - btnHold) > 500)) {
			// repeat at a fixed rate
			if (!btnHold || abs((millis() - btnLastApply) > 50)) {
				// menu
				if (digitalRead(PBN0) == HIGH) {
					if (menuMode)
						cycleMenus();
					else 
						menuMode = 1;
					displayMenu();
				}
				// -
				else if (digitalRead(PBN1) == HIGH) {
					if (menuMode) {
						if (*curMenu->value - curMenu->step >= curMenu->min)
							*curMenu->value -= curMenu->step;
						displayMenu();
					}
				// +
				} else if (digitalRead(PBN2) == HIGH) {
					if (menuMode) {
						if (*curMenu->value + curMenu->step <= curMenu->max)
							*curMenu->value += curMenu->step;
						displayMenu();
					}
				}
				// display
				else if (digitalRead(PBN3) == HIGH) {
					// reset (interferences)
					lcd.begin(16, 2);

					if (menuMode) {
						menuMode = 0;
						saveSettings();
					} else {
						if (!dimDisplay)
							cycleDisplay();
					}
					displayInfo();
				}
				btnLastApply = millis();
				if (!btnHold)
					btnHold = millis();
			}
		}
	} else {
		btnHold = 0;
	}
}


/**********  HELPER FUNCTIONS  **********/
void setValues() {
	myPID.SetTunings(kp,ki,kd);

	analogWrite(PELTIER, powerPeltier);
	analogWrite(FRONTFAN, powerFrontFan);
	analogWrite(BACKFAN, powerBackFan);

	if (dimDisplay && DIMDISPLAYPOWER < powerBackLight) 
		analogWrite(BACKLIGHT, DIMDISPLAYPOWER);
	 else 
		analogWrite(BACKLIGHT, powerBackLight);
}

int deci2(double doub) {
	int t1 = (int)doub;
	return (doub - t1) * 100;
}

void saveSettings() {
	eeprom_write_dword((uint32_t *) (sizeof(uint32_t) * 0), MAGIC_SAVE_NUMBER);
	eeprom_write_dword((uint32_t *) (sizeof(uint32_t) * 1), targetTemp);
	eeprom_write_dword((uint32_t *) (sizeof(uint32_t) * 2), powerBackFan);
	eeprom_write_dword((uint32_t *) (sizeof(uint32_t) * 3), powerBackLight);
	eeprom_write_dword((uint32_t *) (sizeof(uint32_t) * 4), kp);
	eeprom_write_dword((uint32_t *) (sizeof(uint32_t) * 5), ki);
	eeprom_write_dword((uint32_t *) (sizeof(uint32_t) * 6), kd);
}

void loadSettings() {
	if (eeprom_read_dword((uint32_t *) (sizeof(uint32_t) * 0)) == MAGIC_SAVE_NUMBER) {
		targetTemp     = eeprom_read_dword((uint32_t *) (sizeof(uint32_t) * 1));
		powerBackFan   = eeprom_read_dword((uint32_t *) (sizeof(uint32_t) * 2));
		powerBackLight = eeprom_read_dword((uint32_t *) (sizeof(uint32_t) * 3));
		kp             = eeprom_read_dword((uint32_t *) (sizeof(uint32_t) * 4));
		ki             = eeprom_read_dword((uint32_t *) (sizeof(uint32_t) * 5));
		kd             = eeprom_read_dword((uint32_t *) (sizeof(uint32_t) * 6));
	}
}


/**********  MAIN FUNCTIONS  **********/

void setup() {
	// Set Fans PWM to 30kHz
	TCCR1B = TCCR1B & 0b11111000 | 0x01;
	// Set MOSFET PWM to 30Hz
	TCCR2B = TCCR2B & 0b11111000 | 0x07; 

	initMenus();

	Serial.begin(9600);

	lcd.begin(16, 2);
	blankLcdDisplay();

	// Hold display button at boot to prevent saved settings to load
	if (digitalRead(PBN3) != HIGH)
		loadSettings();

	setValues();
    
	if (!findDS(&onewireCave, DSCave)) {
        displayLines("ERROR", "findDS Cave failed");
		while (1) {}
	}
	if (!findDS(&onewireBack, DSBack)) {
        displayLines("ERROR", "findDS Back failed");
		while (1) {}
	}

	myPID.SetMode(AUTOMATIC);
	myPID.SetControllerDirection(REVERSE);
}


void loop(void) {
	if (Serial.available() > 0)
		processSerialCommand();

	processPushBtnInput();

	if (!menuMode) 
		displayInfo();
	if (abs(millis() - btnLastApply) > DIMDISPLAYTIME)
		dimDisplay = 1;
	else
		dimDisplay = 0;

	getTempDS(&onewireCave, DSCave, &tempCave, &lastAskDSCave);
	getTempDS(&onewireBack, DSBack, &tempBack, &lastAskDSBack);

	myPID.Compute();

	setValues();

	delay(50);
}

// TODO : reverse peltier 
// -> SetOutputLimits(-255, 255);
//    quand négatif : renverser la logique 
//
// save float to eeprom
