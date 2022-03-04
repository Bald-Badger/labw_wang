/*
 *
 * CSEE 4840 Lab 2 for 2019
 *
 * Name/UNI: Wang Chen (wc2794)
 */
#include "fbputchar.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "usbkeyboard.h"
#include <pthread.h>

/* Update SERVER_HOST to be the IP address of
 * the chat server you are connecting to
 */
/* arthur.cs.columbia.edu */
#define SERVER_HOST "128.59.19.114"
#define SERVER_PORT 42000

#define BUFFER_SIZE 128
#define TEXTBOX_BUFFER 1024
#define DIALOGUE_BUFFER 2048

#define DISPLAY_COLS 64
#define DISPLAY_ROWS 24
#define DIALOGUE_ROWS 21
#define TEXT_ROWS 2

#define ROWS 24
#define COLS 64
#define TEXT_START_ROW 20

int sockfd;           /* Socket file descriptor */
int dialogue_row = 0; // display server msg on dialogue_row

struct libusb_device_handle *keyboard;
uint8_t endpoint_address;

pthread_t network_thread;
void *network_thread_f(void *);

int get_acsii(const char *, int, int);
void clear_display();
void draw_cutline();
void scroll_textbox(char *, int, int, int);
void scrollup_textbox(char *, int, int, int);

int main()
{
    int err, col;

    struct sockaddr_in serv_addr;

    struct usb_keyboard_packet packet;
    int transferred;
    char keystate[12];

    int cursor_col = 0;
    int cursor_row = DISPLAY_ROWS - TEXT_ROWS;

    char textbox[TEXTBOX_BUFFER];
    int textcount = 0;

    if ((err = fbopen()) != 0)
    {
        fprintf(stderr, "Error: Could not open framebuffer: %d\n", err);
        exit(1);
    }

    /* Draw rows of asterisks across the top and bottom of the screen */
    for (col = 0; col < 64; col++)
    {
        fbputchar('*', 0, col);
        fbputchar('*', 23, col);
    }

    fbputs("Hello CSEE 4840 World!", 4, 10);

    /* Open the keyboard */
    if ((keyboard = openkeyboard(&endpoint_address)) == NULL)
    {
        fprintf(stderr, "Did not find a keyboard\n");
        exit(1);
    }

    /* Create a TCP communications socket */
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        fprintf(stderr, "Error: Could not create socket\n");
        exit(1);
    }

    /* Get the server address */
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, SERVER_HOST, &serv_addr.sin_addr) <= 0)
    {
        fprintf(stderr, "Error: Could not convert host IP \"%s\"\n", SERVER_HOST);
        exit(1);
    }

    /* Connect the socket to the server */
    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        fprintf(stderr, "Error: connect() failed.  Is the server running?\n");
        exit(1);
    }

    // clean screen TODO
    fbputs("Press any key to clean screen", 13, 10); // row col
    for (;;) {
        libusb_interrupt_transfer(keyboard, endpoint_address, (unsigned char *) &packet, sizeof(packet), &transferred, 0);
        if (transferred == sizeof(packet)) {
            sprintf(keystate, "first print %02x %02x %02x", packet.modifiers, packet.keycode[0], packet.keycode[1]);
            printf("%s\n", keystate);
            if ((packet.modifiers != 0x00) || (packet.keycode[0] != 0x00) || (packet.keycode[1] != 0x00)) {
                // 0.1 clean screen
                for (int i = 0; i < ROWS; i++) {
                    for (int j = 0; j < COLS; j++) {
                        fbputchar(' ', i, j);
                    }
                }
                // 0.2 draw first line
                for (int j = 0; j < COLS; j++) {
                    fbputchar('*', TEXT_START_ROW, j);
                }
                break; // only one time
            }
        }
    }

    char cursor;
    cursor = 95; /* ASCII entry, underline serves as cursor */
    fbputchar(cursor, cursor_row, cursor_col);

    /* Start the network thread */
    pthread_create(&network_thread, NULL, network_thread_f, NULL);

    char prevheld_char = 0;
    char debouncing_char = 0;
    char exchange_char = 0; /* char from the second key of prev packet goes to the first key of the next */
    char prev_single = 0;

    for (;;)
    {
        int has_second = 0;
        int input_counts = 0;
        libusb_interrupt_transfer(keyboard, endpoint_address, (unsigned char *)&packet, sizeof(packet), &transferred, 0);


        if (transferred == sizeof(packet))
        {
            sprintf(keystate, "%02x %02x %02x", packet.modifiers, packet.keycode[0], packet.keycode[1]);
            printf("original keystate: %s\n", keystate); // show on the terminal

            /* ------ looking at some special keyboard operations ------ */

            /*
            if (packet.keycode[0] == 0x28 || packet.keycode[1] == 0x28) // first key or second key pressed
            {
                textbox[textcount] = '\0';
                printf("This is textbox: %s\n", textbox);
                write(sockfd, textbox, textcount);
                memset(textbox, '\0', textcount);
                textcount = 0;
                cursor_col = 0;
                cursor_row = DIALOGUE_ROWS + 1;
                for (int row = cursor_row; row < DISPLAY_ROWS; row++)
                {
                    for (int col = 0; col < DISPLAY_COLS; col++)
                    {
                        fbputchar(' ', row, col);
                    }
                }
                continue;
            }*/


            if (packet.keycode[0] == 0x2a)
            { /* backspace pressed */
                prevheld_char = 0;
                debouncing_char = 0;
                exchange_char = 0;
                prev_single = 0;

                /* delete current cursor */
                fbputchar(' ', cursor_row, cursor_col); /* delete */

                if ((cursor_col == 0) && (cursor_row == DISPLAY_ROWS - TEXT_ROWS) && (textcount == 0))
                {
                    continue; /* starting point reached */
                }

                if ((cursor_col == 0) && (cursor_row == DISPLAY_ROWS - 1) && (textcount <= DISPLAY_COLS))
                {
                    cursor_col = DISPLAY_COLS - 1; /* all things left fit in the top line */
                    cursor_row -= 1;
                }
                else if (((cursor_col == 0) && (cursor_row == DISPLAY_ROWS - 1) && (textcount > DISPLAY_COLS)))
                {
                    cursor_col = DISPLAY_COLS - 1;                                 /* let two lines be filled */
                    scrollup_textbox(textbox, textcount, TEXT_ROWS, DISPLAY_ROWS); /* move down one line, and print the first line */
                }
                else
                {
                    cursor_col -= 1;
                }
                fbputchar(' ', cursor_row, cursor_col);    /* delete */
                fbputchar(cursor, cursor_row, cursor_col); /* move cursor to the next spot and display */
                textcount -= 1;
                textbox[textcount] = 0;
                continue;
            }

            /* -------- conditional checks for number of inputs  -------- */
            /* check if there are two key presses from the packet */
            if (packet.keycode[1] != 0x00)
            {
                has_second = 1;
            }

            /* check if it's an end signifier */
            if ((packet.modifiers == 0x00) && (packet.keycode[0] == 0x00) && (packet.keycode[1] == 0x00))
            {
                prevheld_char = 0;
                debouncing_char = 0;
                exchange_char = 0;
                prev_single = 0;
                continue;
            }

            // 1. keystate to char "01 03 04"
            int k1 = get_acsii(keystate, 3, 4);
            int k2 = get_acsii(keystate, 6, 7);

            fbputchar(' ', cursor_row, cursor_col); /* erase cursor */

            /* handle character from the first input */
            if ((prev_single || debouncing_char == 0) && (!has_second || prevheld_char != k1) && (exchange_char != k1))
            {
                input_counts += 1;
                fbputchar(k1, cursor_row, cursor_col); /* display input */
                /* put the first keyboard inputs in a buffer, make it avaliable to be sent */
                textbox[textcount++] = k1;

                prevheld_char = k1;
                exchange_char = 0;
                if (!has_second)
                    prev_single = 1;

                /* move cursor and print cursor on the scrren accordingly */
                if ((cursor_col == DISPLAY_COLS - 1) && (cursor_row < DISPLAY_ROWS - 1))
                {
                    cursor_col = 0; /* index out of the box, line move to the bottom */
                    cursor_row += 1;
                }
                else if ((cursor_col == DISPLAY_COLS - 1) && (cursor_row == DISPLAY_ROWS - 1))
                {
                    cursor_col = 0; /* index out of the box, line reaches the bottom, textbox screws down */
                    scroll_textbox(textbox, textcount, TEXT_ROWS, DISPLAY_ROWS);
                }
                else
                {
                    cursor_col += 1;
                }
            }
            else if ((!has_second) && (debouncing_char != 0) && (exchange_char == k1))
            {
                prev_single = 1; /* debouncing effect takes place here, no input but next input should get ready */
                exchange_char = 0;
            }


            if ((has_second) && (exchange_char != k2))
            {
                input_counts += 1;
                fbputchar(k2, cursor_row, cursor_col);
                /* put the second keyboard input in a buffer */
                textbox[textcount++] = k2;

                debouncing_char = k2;
                prevheld_char = 0;
                exchange_char = k2;
                prev_single = 0;

                /* move cursor and print cursor on the scrren accordingly */
                if ((cursor_col == DISPLAY_COLS - 1) && (cursor_row < DISPLAY_ROWS - 1))
                {
                    cursor_col = 0; /* index out of the box, reset needed */
                    cursor_row += 1;
                }
                else if ((cursor_col == DISPLAY_COLS - 1) && (cursor_row == DISPLAY_ROWS - 1))
                {
                    cursor_col = 0; /* index reaches the bottom, textbox screws down */
                    scroll_textbox(textbox, textcount, TEXT_ROWS, DISPLAY_ROWS);
                }
                else
                {
                    cursor_col += 1;
                }
            }

            fbputchar(cursor, cursor_row, cursor_col); /* move cursor to the next spot and display */
        }
    }
    /* ------------- put keyboard inputs to textbox below ------------- */

    /* Terminate the network thread */
    pthread_cancel(network_thread);

    /* Wait for the network thread to finish */
    pthread_join(network_thread, NULL);

    return 0;
}

void *network_thread_f(void *ignored)
{
    int buff_size = 200;
    char socket_buff[buff_size];
    int n;
    int server_msg_rows; // incoming msg rows
    char dialogueBuf[DIALOGUE_ROWS][DISPLAY_COLS];

    while ((n = read(sockfd, &socket_buff, buff_size - 1)) > 0)
    {
        int col = 0;
        recvBuf[n] = '\0';
        if (n % 4 == 0) server_msg_rows = n / 64;
        else server_msg_rows = n / 64 + 1;

        if (server_msg_rows + dialogue_row > DIALOGUE_ROWS)
        {
            // the msg reaches the input textbox
            int delete_rows = server_msg_rows + dialogue_row - DIALOGUE_ROWS;
            // scroll up
            for (int k = 0; k < delete_rows; k++)
            {
                // delete buffer
                memset(dialogueBuf[k], '\0', sizeof(dialogueBuf[k]));
                // delete screen output
                for (int ck = 0; ck < DISPLAY_COLS; ck++)
                {
                    fbputchar(' ', k, ck);
                }
            }
            int hd = 0;
            for (int j = delete_rows; j < DISPLAY_ROWS; j++)
            {
                // update buffer
                strcpy(dialogueBuf[hd++], dialogueBuf[j]);
                // update screen output
                for (int cj = 0; cj < DISPLAY_COLS; cj++)
                {
                    fbputchar(dialogueBuf[j][cj], hd, cj);
                }
            }
            dialogue_row = hd;
            fbputs(recvBuf, dialogue_row, 0);
            dialogue_row += server_msg_rows; // update dialogue rows to next available row
        }
        else
        {
            fbputs(recvBuf, dialogue_row, 0);
            col = 0;
            for (int i = 0; i < n; i++)
            {
                if (col >= DISPLAY_COLS)
                {
                    dialogue_row++;
                    col = 0;
                }
                dialogueBuf[dialogue_row][col++] = recvBuf[i];
            }
            dialogue_row++;
        }
    }

    return NULL;
}



/* Scroll down the textbox */
void scroll_textbox(char *buffer, int count, int window_size, int lower_bound)
{
    for (int index = 1; index <= window_size; index++)
    {
        for (int col = 0; col < 64; col++)
        {
            fbputchar(' ', lower_bound - index, col); /* clear the row, prepare for rewrite */
        }
    }
    int offset = 0;
    for (int index = 2; index <= window_size; index++)
    {
        for (int col = 63; col >= 0; col--)
        {
            offset += 1;
            char c = buffer[count - offset];
            fbputchar(c, lower_bound - index, col); /* fill the row */
        }
    }
}

/* Scroll up the textbox */
void scrollup_textbox(char *buffer, int count, int window_size, int lower_bound)
{
    for (int index = 1; index <= window_size; index++)
    {
        for (int col = 63; col >= 0; col--)
        {
            fbputchar(' ', lower_bound - index, col); /* clear the rows, prepare for rewrite */
        }
    }
    int offset = 0;
    for (int index = 1; index <= window_size; index++)
    {
        for (int col = 63; col >= 0; col--)
        {
            char c = buffer[count - offset];
            fbputchar(c, lower_bound - index, col); /* fill the row */
            offset += 1;
        }
    }
}


int get_acsii(const char * str, int start, int end) {
    // 1.key code 2 int
    int res = 0;
    int i = start;
    while (i <= end) {
        if ('0' <= str[i] && str[i] <= '9') {
            res  = res * 16 + str[i] - 48;
        } else if ('A' <= str[i] && str[i] <= 'F') {
            res  = res * 16 + str[i] - 55;
        } else if ('a' <= str[i] && str[i] <= 'f') {
            res  = res * 16 + str[i] - 87;
        }
        i += 1;
    }
    // 2. int 2 ascii
    if (res >= 4 && res <= 29) {
        res += 93;
    }
    return res;
}
