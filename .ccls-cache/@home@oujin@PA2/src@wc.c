#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <sys/mman.h>
#include <pthread.h>
#include <stdbool.h>
int32_t fnv_32_str(char *str, int32_t hval)
{
    unsigned char *s = (unsigned char *)str;	/* unsigned string */

    /*
     * FNV-1 hash each octet in the buffer
     */
    while (*s) {

	/* multiply by the 32 bit FNV magic prime mod 2^32 */

	hval += (hval<<1) + (hval<<4) + (hval<<7) + (hval<<8) + (hval<<24);

	/* xor the bottom with the current octet */
	hval ^= (int32_t)*s++;
    }

    /* return our new hash value */
    return hval;
}

typedef struct _HashElem{
	char key[1024];
	uint32_t hash;
} HashElem;

typedef struct _Map{
	HashElem hash;
  struct _Map * nextPtr;
	uint32_t value;
} Map;
Map * globalMap = NULL; 
Map * reduceMap = NULL;
Map * result = NULL;
pthread_mutex_t reduce_mutex;
pthread_mutex_t map_mutex;
int32_t shared_cur;
int32_t reduceN;
int flags = 0;
FILE * fp;
uint32_t limit = 0;
void * stringRemoveNonAlphaNum(char *str){
    int32_t i = 0;
    int32_t j = 0;
    char c;
    while ((c = str[i++]) != '\0')
    {
        if (isalnum(c))
        {
            str[j++] = c;
        }
    }
    str[j] = '\0';
   return str;
}
void allocate_Map() {
	globalMap = (Map *)mmap(0, sizeof(Map)* 0x10000, PROT_READ|PROT_WRITE, MAP_PRIVATE | 0x20, -1, 0);
	//memset((void *)globalMap, 0x0, sizeof(Map)*0x1000000);
  reduceMap = (Map *)mmap(0, sizeof(Map)* 0x100000, PROT_READ|PROT_WRITE, MAP_PRIVATE | 0x20, -1, 0);
	//memset((void *)reduceMap, 0x0, sizeof(Map)*0x10000);
  result = (Map *)mmap(0, sizeof(Map)* 0x100000, PROT_READ|PROT_WRITE, MAP_PRIVATE | 0x20, -1, 0);
	
}

inline int32_t readWord(FILE * fp, char * buf){
  char c;
  char j = 0;
  int32_t r = -1;
  while(!feof(fp)){
    char c = getc(fp);
    if (isalnum(c))
      buf[j++] = tolower(c);
    else{
     buf[j] = '\0';
     return 0;
    }
  }
  buf[j] = '\0';
  return r;
}

void do_Map(int32_t * nWord){
  //printf("START\n");
  char buf[1024];
  for(int i = 0; ; i++){
    pthread_mutex_lock(&map_mutex);
    int32_t index = *nWord;
    if(index >= 0x10000 || readWord(fp, buf) == -1){
      pthread_mutex_unlock(&map_mutex);
      return;
    }
    else if( strlen(buf) == 0){
      pthread_mutex_unlock(&map_mutex);
      continue; 
    }
    strcpy(globalMap[index].hash.key, buf);
    //int32_t hval = index == 0 ? 0x811c9dc5 : globalMap[index -1].hash.hash;
    int32_t hash = fnv_32_str(globalMap[index].hash.key, 0x811c9dc5);
    globalMap[index].hash.hash= hash;
    globalMap[index].value = 1;
    //printf("%d : %s / %08x / %08x\n", index, globalMap[index].hash.key, globalMap[index].hash.hash, globalMap[index].value);
    (*nWord)++;
    pthread_mutex_unlock(&map_mutex);
  }
}
inline Map * findHash(Map * root, Map * target){
  for(Map * cur = root; cur; cur = cur->nextPtr){
    if(cur->hash.hash == target->hash.hash)
      return cur;
  }
  return NULL;
}
void reduceflat(){
  for(int i = 0 ; i < 0x100000; i ++){
    if(reduceMap[i].value){
      memcpy(&result[reduceN++], &reduceMap[i], sizeof(Map));
      for(Map * cur = reduceMap[i].nextPtr; cur; cur = cur->nextPtr){
        memcpy(&result[reduceN++], cur, sizeof(Map));
      }
      //memset(&reduceMap[i],0x00, sizeof(Map));
    }
  }
}
int hi=0;
void do_Reduce(int32_t nWord){
  int32_t max = nWord;
  while(1){
    pthread_mutex_lock(&reduce_mutex);
    if(shared_cur >= nWord){
      pthread_mutex_unlock(&reduce_mutex);
      return;
    }
    
    int32_t i = shared_cur;
    Map * tMap = &globalMap[i];
    int32_t index = tMap->hash.hash &0xffff;
    Map * findMap = &reduceMap[index];
    //printf("[%s]\n",tMap->hash.key);
    if(!findMap->hash.hash){
      hi++;
      memcpy(findMap, tMap, sizeof(Map));
      findMap->nextPtr = NULL;
    }
    else{

      Map * cur = findHash(findMap, tMap);
      if(cur){
        cur->value++;
        
      }
      else{
        hi++;
        Map * cur;
        for(cur = findMap; cur->nextPtr; cur = cur->nextPtr){}
        cur->nextPtr = (Map *)malloc(sizeof(Map));
        cur = cur->nextPtr;
        memcpy(cur, tMap, sizeof(Map));
        cur->nextPtr = NULL;

      }
    }
    shared_cur++;
    pthread_mutex_unlock(&reduce_mutex);
  }
	return;
}
int static compare (const Map* first, const Map* second)
{

    if (first->value > second->value)
        return -1;
    else if (first->value < second->value)
        return 1;
    else
        return strcmp(first->hash.key, second->hash.key);
}

int main(int argc, char ** argv){
	if (argc != 2) {
		fprintf(stderr, "%s: not enough input\n", argv[0]);
		exit(1);
	}
  pthread_t * threads = (pthread_t *)malloc(0x100 * sizeof(pthread_t));
  allocate_Map();
	fp = fopen(argv[1], "r");
  fseek(fp, 0, SEEK_END);
  int fileLength = ftell(fp);
  rewind(fp);
  int32_t nThread = 14;//atoi(argv[2]);
  limit = 1;
  if( fileLength > 0x10000){
    limit = (fileLength / 0x10000) + 1; 
  }
  printf("%d / %d\n", fileLength, limit);
  for(int32_t i = 0; i < limit; i++){
    pthread_mutex_init(&map_mutex, NULL);
    pthread_mutex_init(&reduce_mutex, NULL);
    shared_cur = 0;
    int32_t index =0;
    int32_t nWord = 0;
    
    for(int32_t i = 0; i < nThread; i++)
      pthread_create(&threads[i], NULL, do_Map, &nWord);
    
    for(int32_t i = 0; i < nThread; i++)
      pthread_join(threads[i], NULL);
    if(nWord == 0)
      break;
    //printf("complete map\n");
    for(int32_t i = 0; i < nThread; i++)
      pthread_create(&threads[i], NULL, do_Reduce, nWord);
    
    for(int32_t i = 0; i < nThread; i++)
      pthread_join(threads[i], NULL);
    //printf("%d\n",i);
    //printf("%d : 0x%08x\n",i,hi);
    //printf("complete reduce\n");
  }
  printf("complete\n");
  reduceflat();
  qsort(result, reduceN, sizeof(Map), compare);
  
  for(int32_t i = 0; i < reduceN; i++)
    printf("%s %d\n", result[i].hash.key, result[i].value);	
  
  /*
  while(fscanf(fp, "%s", buf) != EOF)
    printf("[%s]\n", buf);
  */
}
