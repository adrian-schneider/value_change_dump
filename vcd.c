#include <ctype.h>
#include <inttypes.h>  // for scanf(u64) portability
#include <stdint.h>    // u64 typedef
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define USAGE "USAGE: vcd < in.vcd > out.ascii :\n"
#define PROLOG "Fatal error. Can't continue."
#define REBUILD(D) #D " reached (" VAL(D) "), rebuild with -D" #D "=...\n"
#define die(...) exit(fprintf(stderr, PROLOG "\nReason: " __VA_ARGS__))

#define SFL 127  // scanf Token limit
#ifndef MAX_SCOPE
#define MAX_SCOPE 32  // how many char scopes[] to allocate
#endif
#ifndef MAX_CHANNEL
#define MAX_CHANNEL 63  // how many Channels + 1 to allocate 96*96 = 8836
#endif
#define ITV_TIME 10                 // sample interval to display timestamp
#define VALUES "0123456789zZxXbU-"  // allowed bus values/types
#define COUNT(A) (sizeof(A) / sizeof(*A))
#define MAX(A, B) (A > B ? A : B)
#define VAL(A) #A
#define TXT(A) VAL(A)

typedef char Token[SFL + 1];  // parsing token (channel name, scope name, ...)
typedef struct {
  char *low, *raise, *high, *drown, *start, *end;
  unsigned skip;
} PrintOpt;
typedef struct {
  // could have as much state as it bus size but nobody handle such case
  // 'UZX' => show state
  // '\0' => show .val
  char state;
  unsigned val;
} Sample;
typedef struct {
  unsigned size;
  unsigned scope;
  Token name;
  Sample* samples;
} Channel;

typedef struct {
  Channel ch[MAX_CHANNEL + 1];  // [0] = timestamps
  Token scopes[MAX_SCOPE];  // [0] = default
  unsigned total, scope_count, chan_str;
  float scale;                // duration of each sample
  Token date, version, unit;  // file info
  // parsing related values
  unsigned scope_cur;
  unsigned scope_lim, ch_lim, sz_lim;
} ParseCtx;

#define MAX_TOKEN_LENGTH 5
#define SPACE_FOR_TOKENS(n) ((n) * MAX_TOKEN_LENGTH + 1)
#define MAX_TOKEN_DICT_LENGTH SPACE_FOR_TOKENS(MAX_CHANNEL + 1)
#define TOKEN_SEPARATOR ' '

int g_debug = 0;
#define DBGERR if (g_debug) { fprintf(stderr,
#define DBGEND ); } 

char g_token_dict[MAX_TOKEN_DICT_LENGTH] = "";

/* 
Lookup the token in the token dictionary token_dict.
If not found add the token to the dictionary.
Return the token's index 0..n-1 or
  -1 if the token is too long.
  -2 if the token dictionary is full.
*/
int lookup(char *token_dict, const char *token) {
  int rc = -1;

  if (strnlen(token, 2 * MAX_TOKEN_LENGTH) <= MAX_TOKEN_LENGTH) {
    char tks[2]; tks[0] = TOKEN_SEPARATOR; tks[1] = '\0';

    /* Token + two spaces + \0. */
    char padded_token[MAX_TOKEN_LENGTH + 3] = "";
    strcat(padded_token, tks);
    strncat(padded_token, token, MAX_TOKEN_LENGTH);
    strcat(padded_token, tks);

    /* Initialize the token dictionary. */
    if (! *token_dict) {
      strcat(token_dict, tks);
    }
    DBGERR "lookup - /%s/ in /%s/ ", padded_token, token_dict DBGEND

    char *found_at = NULL;
    char *td = token_dict;
    if ((found_at = strstr(token_dict, padded_token))) {
      /* Count token separators until the found_at position. */
      for (rc = 0; td != found_at; *td == *tks ? rc++ : 0, td++);
    }
    else {
      rc = -2; /* Token dict is too short to append the token. */
      size_t needed_len = strlen(token_dict) + strlen(padded_token) - 1;
      if (needed_len <= MAX_TOKEN_DICT_LENGTH) {
        /* Count all token separators - 1. */
        for (rc = -1; *td; *td == *tks ? rc++ : 0, td++);
        /* Append the padded token minus its leading space. */
        strncat(token_dict, padded_token + 1, MAX_TOKEN_LENGTH + 1);
      }
    } 
  }
  
  DBGERR "rc: %d\n", rc DBGEND
  
  return rc;
}

#define MAX_INTEN 7
#define MAX_INTEN_LEN 32
#define INTEN_OFF -1

#define RANGE_SEPARATOR '-'

int limit0Max(int x, int max) { return abs(x) % ((max) + 1); }

/*
Parse a range and return true if successful.
Valid arg strings and returned value for rangeFrom, rangeTo:
  9*99  Value 9, 99.
  -99   Value 0, 99.
  9*    Value 9, rangeMax.
  9     Value 9 for both.
With * as the range separator. A '-' is a natural choice.
Returned values are from 0 to n% (rangeMax + 1).
*/
int parseRange(const char *arg, int *rangeFrom, int *rangeTo, int rangeMax) {
  *rangeFrom = 0;
  *rangeTo = rangeMax;
  char rangeSepar = RANGE_SEPARATOR;

  int rc = sscanf(arg, "%d%c%d", rangeFrom, &rangeSepar, rangeTo);

  if ((rc > 0) && (rangeSepar == RANGE_SEPARATOR)) {
    if (*rangeFrom < 0) {
      *rangeTo = limit0Max(*rangeFrom, rangeMax);
      *rangeFrom = 0;
    }
    else {
      *rangeFrom = limit0Max(*rangeFrom, rangeMax);
      *rangeTo = (rc < 2) ? *rangeFrom : limit0Max(*rangeTo, rangeMax);
    }
    return 1;
  }
  return 0;
}

#define ARG_VALUE_LEN 4
#define ARG_SEPAR_RANGE '/'

int parseInten(const char *arg, int *rangeFrom, int *rangeTo, int *inten) {
  *inten = -1;
  char argSepar = ARG_SEPAR_RANGE;
  char argValue[ARG_VALUE_LEN + 1] = "";

  char fmt[16];
  sprintf(fmt, "%%d%%c%%%ds", ARG_VALUE_LEN);
  DBGERR "parseInten - fmt:%s\n", fmt DBGEND

  int rc = sscanf(arg, fmt, inten, &argSepar, argValue);
  *inten = limit0Max(*inten, MAX_INTEN);
  DBGERR "parseInten - inten:%d separ:%c value:%s\n", *inten, argSepar, argValue DBGEND

  if (rc > 0) {
    if (argSepar == ARG_SEPAR_RANGE) {
      return parseRange(argValue, rangeFrom, rangeTo, MAX_CHANNEL);
    }
  }

  return 0;
}

char g_intenOn[MAX_INTEN + 1][MAX_INTEN_LEN];
char g_intenOff[MAX_INTEN_LEN];
int g_waveInten[MAX_CHANNEL + 1];

void intensifyWave(int rangeFrom, int rangeTo, int inten) {
  for(int i = rangeFrom; i <= rangeTo; i++) { g_waveInten[i] = inten; }
}

void initInten(int fmtHtml) {
  strcat(g_intenOn[0], fmtHtml ? "<span style=\"color: black\">"   : "\033[30m");
  strcat(g_intenOn[1], fmtHtml ? "<span style=\"color: red\">"     : "\033[31m");
  strcat(g_intenOn[2], fmtHtml ? "<span style=\"color: green\">"   : "\033[32m");
  strcat(g_intenOn[3], fmtHtml ? "<span style=\"color: yellow\">"  : "\033[33m");
  strcat(g_intenOn[4], fmtHtml ? "<span style=\"color: blue\">"    : "\033[34m");
  strcat(g_intenOn[5], fmtHtml ? "<span style=\"color: magenta\">" : "\033[35m");
  strcat(g_intenOn[6], fmtHtml ? "<span style=\"color: cyan\">"    : "\033[36m");
  strcat(g_intenOn[7], fmtHtml ? "<span style=\"color: white\">"   : "\033[37m");
  strcat(g_intenOff,   fmtHtml ? "</span>"                         : "\033[0m");
}

char g_htmlPrefix[1024];
char g_htmlSuffix[1024];

void initHtml(int invert) {
  char *hivrt = getenv("HIVRT") ?: "style=\"background-color:black;color:white\"";
  char *hfont = getenv("HFONT") ?: "style=\"font-family:'Cascadia Mono','Menlo',monospace;line-height:%s;\"";
  char *hlinh = getenv("HLINH") ?: "1.2";
  strcat(g_htmlPrefix, "<html><body");
  strcat(g_htmlPrefix, (invert ? " " : ""));
  strcat(g_htmlPrefix, (invert ? hivrt : ""));
  strcat(g_htmlPrefix, (">\n<pre "));
  strcat(g_htmlPrefix, hfont);
  strcat(g_htmlPrefix, ">\n");
  strcat(g_htmlSuffix, "</pre></body></html>\n");
}

char *intenOnChan(int chan) { return g_waveInten[chan] > INTEN_OFF ? g_intenOn[g_waveInten[chan]] : ""; }
char *intenOffChan(int chan) { return g_waveInten[chan] > INTEN_OFF ? g_intenOff : ""; }

/*
convert a base-94 or 'c'+num chan id (!...~) to integer
This has been replaced by a dynamic dictionary.
*/
size_t chanId(char* str_id, unsigned isStr) {
  size_t id = 0;
  if (isStr) {
    id = atoi(str_id + 1);
  } else {
    int rc = lookup(g_token_dict, str_id);
    if (rc < 0) {
      die("Lookup failed with %d for id %s", rc, str_id);
    }
    else {
      id = rc;
    }
  /*     
    for (size_t i = strlen(str_id); i >= 1; i--) {
      id = (id * 94) + str_id[i - 1] - '!';
    }
  */  }
  if (id > MAX_CHANNEL) die(REBUILD(MAX_CHANNEL));
  return id;
}

size_t unilen(char* s) {
  size_t j = 0;
  for (; *s; s++) j += ((*s & 0xc0) != 0x80);
  return j;
}

/* read a $instruction and it opt if needed until $end*/
void parseVcdInstruction(ParseCtx* p) {
  Token token;
  scanf("%" TXT(SFL) "s", token);
  if (!strcmp("var", token)) {
    Token id;
    Channel c = {.scope = p->scope_cur, .samples = malloc(sizeof(Sample))};
    scanf(" %*s %u %" TXT(SFL) "[^ ] %" TXT(SFL) "[^$]", &c.size, id, c.name);
    p->ch_lim = MAX(p->ch_lim, strlen(c.name));
    p->sz_lim = MAX(p->sz_lim, c.size);
    p->ch[chanId(id, p->chan_str)] = c;
  } else if (!strcmp("scope", token)) {
    p->scope_count++;
    if (p->scope_count == MAX_SCOPE) die(REBUILD(MAX_SCOPE));
    p->scope_cur = p->scope_count;
    scanf("%*s %" TXT(SFL) "[^ $]", p->scopes[p->scope_cur]);
    p->scope_lim = MAX(p->scope_lim, strlen(p->scopes[p->scope_cur]));
  } else if (!strcmp("date", token)) {
    scanf("\n%" TXT(SFL) "[^$\n]", p->date);
  } else if (!strcmp("version", token)) {
    scanf("\n%" TXT(SFL) "[^$\n]", p->version);
    // ROHD use 's'+digit channel ID sequencing
    p->chan_str = strstr(p->version, "ROHD") != NULL;
  } else if (!strcmp("timescale", token)) {
    scanf("\n%f%" TXT(SFL) "[^$\n]", &p->scale, p->unit);
  } else if (!strcmp("comment", token)) {
    scanf("\n%*[^$]");
  } else if (!strcmp("upscope", token)) {
    scanf("\n%*[^$]");
    p->scope_cur = 0;  // back to the root
  } else if (!strcmp("enddefinitions", token)) {
    scanf("\n%*[^$]");
  } else if (!strcmp("dumpvars", token)) {
  } else if (!strcmp("end", token)) {
  } else {
    printf("unknown token : %s\n", token);
  }
}
/* Parse a time line (ex: '#210000000') and copy all previous samples values */
void parseVcdTimestamp(ParseCtx* p) {
  // copy previous sample on every channel
  if (p->total > 0) {
    for (Channel* ch = p->ch; ch < p->ch + COUNT(p->ch); ch++) {
      if (!ch->size) continue;  // skip unused channels
      ch->samples = realloc(ch->samples, sizeof(Sample) * (p->total + 1));
      ch->samples[p->total] = ch->samples[p->total - 1];
    }
  }
  uint64_t _unused;
  scanf("%" PRIu64, &_unused);  // p->timestamps[p->total]
  p->total++;
}
/*
sample line end with the channel ID and start either with a state or data:
1^
Z^
b0100 ^
0! 0" 1# 0$ 1% 0& 1'
*/
void parseVcdSample(ParseCtx* p, int c) {
  Sample s = {'\0', 0};
  if (c == 'b') {
    for (c = getchar(); c != EOF && c != ' '; c = getchar()) {
      if (c == '0' || c == '1') {
        s.val = s.val * 2 + (c - '0');
      } else if (strchr(VALUES, c)) {
        s.state = c;
      } else {
        die("Unknown sample value: %c", c);
      }
    }
  } else {
    s.state = isalpha(c) ? c : '\0';
    s.val = isdigit(c) ? c - '0' : 0;
  }
  Token id_str;
  scanf("%" TXT(SFL) "[^ \n]", id_str);
  if (!p->total) return;  // ROHD define value BEFORE timestamp #0
  p->ch[chanId(id_str, p->chan_str)].samples[p->total - 1] = s;
}

void parseVcd(ParseCtx* p) {
  for (int c = getchar(); c != EOF; c = getchar()) {
    if (isspace(c)) continue;
    if (c == '$') {
      parseVcdInstruction(p);
    } else if (c == '#') {
      parseVcdTimestamp(p);
    } else if (strchr(VALUES, c)) {
      parseVcdSample(p, c);
    } else {
      die("unknow char : %c\n", c);
    }
  }
}

void printYml(ParseCtx* p, PrintOpt* opt, int fmtHtml) {
  if (unilen(opt->high) != 1) die("high waveform length must be 1");
  if (unilen(opt->low) != 1) die("low waveform length must be 1");
  if (unilen(opt->drown) > 1) die("drown waveform length must be 1 or empty");
  if (unilen(opt->raise) > 1) die("raise waveform length must be 1 or empty");

  if (fmtHtml) { printf("%s", g_htmlPrefix); }

  int zoom = (p->sz_lim + 7) >> 2;  // how many char per sample (8bit => 2)
  int trans = *opt->drown && *opt->raise;
  printf("global:\n");
  printf("  zoom:    %i\n", zoom);
  printf("  date:    %s\n", p->date);
  printf("  total:   %i\n", p->total);
  printf("  skip:    %i\n", opt->skip);
  printf("  time:\n");
  printf("    scale: %.2f\n", p->scale);
  printf("    unit:  %s\n", p->unit ?: "?");
  printf("    %-*s: %s", p->ch_lim, "line", opt->start);
  for (double smpl = opt->skip; smpl < p->total; smpl += ITV_TIME) {
    printf("%-*g ", ITV_TIME * zoom - 1, smpl * p->scale);
  }
  printf("%s\nchannels:\n", opt->end);
  int chan = 0;
  for (Channel* ch = p->ch; ch - p->ch < (signed)COUNT(p->ch); ch++) {
    // skip empty ch
    if (!ch->size) continue;
    // print scope (if changed)
    if (ch == p->ch || ch->scope != ((ch - 1)->scope)) {
      printf("  %s:\n", ch->scope ? p->scopes[ch->scope] : "default");
    }

    printf("    %-*s: %s", p->ch_lim, ch->name, opt->start);
    printf("%s", intenOnChan(chan));
    for (Sample* s = ch->samples + opt->skip; s < ch->samples + p->total; s++) {
      Sample* prev = s > ch->samples ? s - 1 : s;
      if (s->state) {  // state data: UUUUZZZZ-
        printf("%-*c", zoom, s->state);
      } else if (ch->size == 1) {  // binary wave: ▁▁/▔▔
        // have a different data => print a transition
        for (int w = 0; w < zoom; w++) {
          if (!w && trans && s->state == prev->state && s->val != prev->val)
            printf("%s", prev->val ? opt->drown : opt->raise);
          else
            printf("%s", s->val ? opt->high : opt->low);
        }
      } else {  // bus : show hex value
        printf("%-*X", zoom, s->val);
      }
    }
    printf("%s", intenOffChan(chan)); chan++;
    printf("%s\n", opt->end);
  }

  if (fmtHtml) { printf("%s", g_htmlSuffix); }
}

void printUsage(char* progname) {
  fprintf(stderr, "Usage: %s [options] [< infile]\n", progname);
  fprintf(stderr, "       %s [options] [< infile] [> outfile]\n", progname);
  fprintf(stderr, "       %s [options] [< infile] [| program]\n", progname);
  fprintf(stderr, "  -7        Use only ASCII characters in the output.\n");
  fprintf(stderr, "  -H        Output as html with font setting.\n");
  fprintf(stderr, "  -h        Show this command usage.\n");
  fprintf(stderr, "  -i c/n-m  Intensify waves n to m with color c.\n");
  fprintf(stderr, "  Environment variables:\n");
  fprintf(stderr, "  STX   Start prefix text.\n");
  fprintf(stderr, "  ETX   End suffix text. Examples:\n");
  fprintf(stderr, "        STX=$(printf \"\\x1b[32m\") ETX=$(printf \"\\x1b[0m\")\n");
  fprintf(stderr, "          Print signals in green (ANSI) color.\n");
  fprintf(stderr, "        RAISE=\"\" DROWN=\"\"\n");
  fprintf(stderr, "          Disable RAISE/DROWN tranistion.\n");
  fprintf(stderr, "  SKIP  Skip initial number of samples.\n");
  fprintf(stderr, "  RAISE Signal raise character.\n");
  fprintf(stderr, "  DROWN Signal drown character.\n");
  fprintf(stderr, "  LOW   Signal low character. Not empty.\n");
  fprintf(stderr, "  HIGH  Signal high character. Not empty.\n");
  exit(EXIT_FAILURE);
}

int main(int argc, char* argv[]) {
  int cmdopt;
  int useAscii = 0;
  int fmtHtml = 0;
  int invert = 0;

  intensifyWave(0, MAX_CHANNEL, INTEN_OFF);

  while ((cmdopt = getopt(argc, argv, "7Hhc:i:IdD")) != -1) {
    int rangeFrom = -1, rangeTo = -1, inten = -1;
    switch (cmdopt) {
      case '7': useAscii = 1; break;
      case 'H': fmtHtml = 1; break;
      case 'i':
        if (parseInten(optarg, &rangeFrom, &rangeTo, &inten)) {
          DBGERR "  rangeFrom:%d rangeTo:%d inten:%d\n", rangeFrom, rangeTo, inten DBGEND
          intensifyWave(rangeFrom, rangeTo, inten);
        }
        break;
      case 'I': invert = 1; break;
      case 'd': g_debug = 1; break;
      case 'D': g_debug = 0; break;
      case 'h':
      default: printUsage(argv[0]);
    }
  }

  initInten(fmtHtml);
  initHtml(invert);

  PrintOpt opt;
  opt = (PrintOpt) {
    getenv("LOW") ?: "▁",       getenv("RAISE") ?: "╱",
    getenv("HIGH") ?: "▔",      getenv("DROWN") ?: "╲",
    getenv("STX") ?: "\"",      getenv("ETX") ?: "\"",
    atoi(getenv("SKIP") ?: "0")
  };
  if (useAscii) {
    opt = (PrintOpt) {
      getenv("LOW") ?: "_",       getenv("RAISE") ?: "/",
      getenv("HIGH") ?: "#",      getenv("DROWN") ?: "\\",
      getenv("STX") ?: "\"",      getenv("ETX") ?: "\"",
      atoi(getenv("SKIP") ?: "0")
    };
  }
  // PrintOpt opt = {"_", "/", "#", "\\"} {"▁", "╱", "▔", "╲"};
  ParseCtx ctx = {0};
  parseVcd(&ctx);
  printYml(&ctx, &opt, fmtHtml);
  return 0;
}
