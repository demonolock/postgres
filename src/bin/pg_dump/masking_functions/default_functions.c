#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char
getRandom(int lower, int upper)
{
    return (char) (rand() % (upper - lower + 1)) + lower;
}

// Saved Currency symbols
// ' ', '.', ',', '-', '@' to save the structure
// 0

char *
default_char(char *old_str)
{
    srand(time(0));
    int i = 0;
    char *s = malloc(sizeof(char));
    strcpy(s, old_str);
    do
    {
        // ASCII
        if (s[i] >= 'A' && s[i] <= 'Z') // [A-Z]
        {
            s[i]=getRandom('A', 'Z');
        }
        else if (s[i] >= 'a' && s[i] <= 'z') // [a-z]
        {
            s[i]=getRandom('a', 'z');
        }
        else if (s[i] >= '!' && s[i] <= '+' || s[i] > '9' && s[i] < '@' || s[i] > 'Z' && s[i] < 'a' || s[i] > 'z' && s[i] <= '~') // Symbols
        {
            s[i]=getRandom('!', '+'); // Leave ' ', '.', ',', '-' to save the structure
        }
        else if (s[i] >0 && s[i] <=9) // Digits (save 0, to avoid situations, where with float numbers)
        {
            s[i]=getRandom('1', '9');
        }
//        // UTF-8
//        else if (s[i]>=256 && s[i]<=591) // Extended latin-a and latin-b
//        {
//            s[i]=getRandom(256, 591);
//        }
//        else if (s[i]>=688 && s[i]<=767) // Dpace modificators
//        {
//            s[i]=getRandom(688, 767);
//        }
//        else if (s[i]>=768 && s[i]<=879) // Diactritic symbols
//        {
//            s[i]=getRandom(768, 879);
//        }
//        else if (s[i]>=880 && s[i]<=1023) // Greek and Coptic
//        {
//            s[i]=getRandom(880, 1023);
//        }
//        else if (s[i]>=1024 && s[i]<=1279) // Basic cyrillic
//        {
//            s[i]=getRandom(1024, 1279);
//        }
//        else if (s[i]>=1280 && s[i]<=1327) // Extended cyrillic
//        {
//            s[i]=getRandom(1280, 1327);
//        }
//        else if (s[i]>=8192 && s[i]<=8303) // Punctuation marks
//        {
//            s[i]=getRandom(8192, 8303);
//        }
//        else if (s[i]>=8448 && s[i]<=8527) // Letter-like symbols
//        {
//            s[i]=getRandom(8448, 8527);
//        }
//        else if (s[i]>=8592 && s[i]<=8703) // Arrows
//        {
//            s[i]=getRandom(8592, 8703);
//        }
//        else if (s[i]>=8704 && s[i]<=8959) // Math operators
//        {
//            s[i]=getRandom(8704, 8959);
//        }
//        else if (s[i]>=9472 && s[i]<=9599) // Pseudo-graphic symbols
//        {
//            s[i]=getRandom(9472, 9599);
//        }
//        else if (s[i]>=9600 && s[i]<=9631) // Blocks
//        {
//            s[i]=getRandom(9600, 9631);
//        }
//        else if (s[i]>=9632 && s[i]<=9727) // Geometric figures
//        {
//            s[i]=getRandom(9632, 9727);
//        }
//        else if (s[i]>=9728 && s[i]<=9983) // Other symbols
//        {
//            s[i]=getRandom(9728, 9983);
//        }
//        else if (s[i]>=9984 && s[i]<=10176) // Graphical icons
//        {
//            s[i]=getRandom(9984, 10176);
//        }

        i++;
    } while (s[i]!='\0');

    printf("Old: `%s`, New: `%s`\n", old_str, s);
    return s;
}
