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
#define SMALL_BUFFER_SIZE 48 /* 47 + 1 null terminator */
#define TEXTBOX_BUFFER 1024
#define DIALOGUE_BUFFER 2048

#define DISPLAY_COLS 64
#define DISPLAY_ROWS 24
#define DIALOGUE_ROWS 21
#define TEXT_ROWS 2

/*
 * References:
 *
 * http://beej.us/guide/bgnet/output/html/singlepage/bgnet.html
 * http://www.thegeekstuff.com/2011/12/c-socket-programming/
 *
 */

int sockfd;           /* Socket file descriptor */
int dialogue_row = 0; // display server msg on dialogue_row

struct libusb_device_handle *keyboard;
uint8_t endpoint_address;

pthread_t network_thread;
void *network_thread_f(void *);

void clear_display();
void draw_cutline();
void scroll_textbox(char *, int, int, int);
void scrollup_textbox(char *, int, int, int);
int hex2int(char *);
void slice_str(const char *, char *, size_t, size_t);
char dec2chr(int);
char handle_modifier(int, int);

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

    /* ------------------------ code added below ------------------------ */
    /* Read inputs from socket and print before the program starts */
    /* incoming information shouldn't exceed BUFFER_SIZE bits */
    char tempBuf[SMALL_BUFFER_SIZE];
    int n;
    int line_num = 20;

    /* reading and printing message from server before it starts */
    n = read(sockfd, &tempBuf, SMALL_BUFFER_SIZE - 1);
    tempBuf[n] = '\0'; /* make sure the string is null-terminated */
    printf("%s", tempBuf);
    /* divide message been read into multiple lines */
    if (line_num < DISPLAY_ROWS)
    {
        fbputs(tempBuf, line_num, 10);
        line_num += 1;
    }

    fbputs("Press Any Key to Start", 12, 10);

    /* Program initialization upon pressing a key */
    for (;;)
    {
        libusb_interrupt_transfer(keyboard, endpoint_address,
                                  (unsigned char *)&packet, sizeof(packet),
                                  &transferred, 0);
        if (transferred == sizeof(packet))
        {
            sprintf(keystate, "%02x %02x %02x", packet.modifiers, packet.keycode[0],
                    packet.keycode[1]);
            printf("%s\n", keystate);
            if ((packet.modifiers != 0x00) | (packet.keycode[0] != 0x00) |
                (packet.keycode[1] != 0x00))
            { /* a key pressed */
                clear_display();
                draw_cutline();
                break;
            }
        }
    }

    char cursor;
    cursor = 95; /* ASCII entry, underline serves as cursor */
    fbputchar(cursor, cursor_row, cursor_col);

    /* ------------------------ code added above ------------------------ */

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

        libusb_interrupt_transfer(keyboard, endpoint_address,
                                  (unsigned char *)&packet, sizeof(packet), &transferred, 0);
        if (transferred == sizeof(packet))
        {
            sprintf(keystate, "%02x %02x %02x", packet.modifiers, packet.keycode[0], packet.keycode[1]);
            printf("original keystate: %s\n", keystate); // show on the terminal

            /* ------ looking at some special keyboard operations ------ */

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
            }

            if (packet.keycode[0] == 0x29)
            { /* ESC pressed */
                break;
            }

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

            if (packet.keycode[0] == 0x5c)
            { /* left arrow pressed */
            }

            if (packet.keycode[0] == 0x5e)
            { /* right arrow pressed */
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

            /* ------------- put keyboard inputs to textbox below ------------- */
            char ck1, ck2;
            char modifier[3], key1[3], key2[3];
            slice_str(keystate, modifier, 0, 1);
            slice_str(keystate, key1, 3, 4);
            slice_str(keystate, key2, 6, 7);

            int m = hex2int(modifier), k1 = hex2int(key1), k2 = hex2int(key2);

            printf("hex: %s, dec: %d\n", modifier, m);
            printf("hex: %s, dec: %d\n", key1, k1);
            printf("hex: %s, dec: %d\n", key2, k2);

            fbputchar(' ', cursor_row, cursor_col); /* erase cursor */
            ck1 = dec2chr(k1);
            ck1 = handle_modifier(m, ck1);

            /* handle character from the first input */
            if ((prev_single || debouncing_char == 0) && (!has_second || prevheld_char != ck1) && (exchange_char != ck1))
            {
                input_counts += 1;
                fbputchar(ck1, cursor_row, cursor_col); /* display input */
                /* put the first keyboard inputs in a buffer, make it avaliable to be sent */
                textbox[textcount++] = ck1;

                prevheld_char = ck1;
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
            else if ((!has_second) && (debouncing_char != 0) && (exchange_char == ck1))
            {
                prev_single = 1; /* debouncing effect takes place here, no input but next input should get ready */
                exchange_char = 0;
            }

            /* handle character from the second input */
            if (has_second)
            {
                ck2 = dec2chr(k2);
                ck2 = handle_modifier(m, ck2);
            }
            if ((has_second) && (exchange_char != ck2))
            {
                input_counts += 1;
                fbputchar(ck2, cursor_row, cursor_col);
                /* put the second keyboard input in a buffer */
                textbox[textcount++] = ck2;

                debouncing_char = ck2;
                prevheld_char = 0;
                exchange_char = ck2;
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
    char recvBuf[BUFFER_SIZE];
    int n;
    int server_msg_rows; // incoming msg rows
    char dialogueBuf[DIALOGUE_ROWS][DISPLAY_COLS];
    /* Receive data */
    while ((n = read(sockfd, &recvBuf, BUFFER_SIZE - 1)) > 0)
    {
        printf("thread is listening...\n");
        int col = 0;
        recvBuf[n] = '\0';
        printf("has listened: %s\n", recvBuf);
        if (n % 4 == 0)
        {
            server_msg_rows = n / 64;
        }
        else
        {
            server_msg_rows = n / 64 + 1;
        }

        // printf("%s", recvBuf);

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

/* ------------------------ extra helper functions below ------------------------ */
/* Clear everything in the display */
void clear_display()
{
    for (int row = 0; row < DISPLAY_ROWS; row++)
    {
        for (int col = 0; col < DISPLAY_COLS; col++)
        {
            fbputchar(' ', row, col); /* clear the row, prepare for rewrite */
        }
    }
}

/* Draw cutline between dialogue box and textbox */
void draw_cutline()
{
    /* Draw rows of asterisks across dialogue window and textbox */
    for (int col = 0; col < 64; col++)
    {
        fbputchar('*', DISPLAY_ROWS - TEXT_ROWS - 1, col);
    }
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

/* hexadecimal to decimal conversion */
int hex2int(char *hex)
{
    int decimal = 0, base = 1;
    int length = strlen(hex);
    for (int i = length--; i >= 0; i--)
    {
        if (hex[i] >= '0' && hex[i] <= '9')
        {
            decimal += (hex[i] - 48) * base;
            base *= 16;
        }
        else if (hex[i] >= 'A' && hex[i] <= 'F')
        {
            decimal += (hex[i] - 55) * base;
            base *= 16;
        }
        else if (hex[i] >= 'a' && hex[i] <= 'f')
        {
            decimal += (hex[i] - 87) * base;
            base *= 16;
        }
    }
    return decimal;
}

void slice_str(const char *str, char *buffer, size_t start, size_t end)
{
    size_t j = 0;
    for (size_t i = start; i <= end; ++i)
    {
        buffer[j++] = str[i];
    }
    buffer[j] = 0;
}

/* ASCII index conversion */
char dec2chr(int key)
{
    if (key >= 4 && key <= 29)
    {
        key += 93; // lower case by default
    }
    else if (key >= 30 && key <= 38)
    {
        key += 19; // numerical conversion of 30 to 38
    }
    else if (key == 39)
    {
        key = 48; // numerical conversion of 0
    }
    else if (key == 44)
    {
        key = 32; // space
    }
    return key;
}

char handle_modifier(int m, int k)
{
    if (m == 2)
    { // modifier consider only shift (cap letter)
        k -= 32;
    }
    return k;
}

