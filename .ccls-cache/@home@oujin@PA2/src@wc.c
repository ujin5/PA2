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
	char key[512];
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

inline int32_t readLine(FILE *fp, char * buf){

  int32_t s = fgets(buf, 4096, fp);
  return s;
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
    (*nWord)++;
    pthread_mutex_unlock(&map_mutex);
    strcpy(globalMap[index].hash.key, buf);
    //int32_t hval = index == 0 ? 0x811c9dc5 : globalMap[index -1].hash.hash;
    int32_t hash = fnv_32_str(globalMap[index].hash.key, 0x811c9dc5);
    globalMap[index].hash.hash= hash;
    globalMap[index].value = 1;
    //printf("%d : %s / %08x / %08x\n", index, globalMap[index].hash.key, globalMap[index].hash.hash, globalMap[index].value);
    
    
  }

}
inline Map * findHash(Map * root, Map * target){
  for(Map * cur = root; cur; cur = cur->nextPtr){
    if(cur->hash.hash == target->hash.hash)
      return cur;
  }
  return NULL;
}
int32_t nResult = 0;
int32_t allWord = 0;
void reduceflat(){
  for(int i = 0 ; i < 0x100000; i ++){
    if(reduceMap[i].value){
      memcpy(&result[reduceN++], &reduceMap[i], sizeof(Map));
      for(Map * cur = reduceMap[i].nextPtr; cur; cur = cur->nextPtr){
        memcpy(&result[reduceN++], cur, sizeof(Map));

      }
      if(reduceN == allWord)
        break;
      //memset(&reduceMap[i],0x00, sizeof(Map));
    }
  }
}
struct _thread_arg{
  int32_t nWord;
  int32_t position;
  int32_t max;
};
int32_t coll = 0;
int32_t dup = 0;
void do_Reduce(struct _thread_arg * args){

  int32_t nWord = args->nWord;
  int32_t position = args->position;
  int32_t i = 0;
  //printf("%d %d %d \n", args->nWord, args->position, args->max);
  while(1){
    if(i >= args->max ){
      return;
    }
    
    pthread_mutex_lock(&reduce_mutex);
    Map * tMap = &globalMap[i+position];
    int32_t index = tMap->hash.hash &0xfffff;
    Map * findMap = &reduceMap[index];

   
    //printf("[%s]\n",tMap->hash.key);
    if(!findMap->hash.hash){
      allWord++;
      memcpy(findMap, tMap, sizeof(Map));
      findMap->nextPtr = NULL;
    }
    else{
      
      Map * cur = findHash(findMap, tMap);
      if(cur){
        dup++;
        cur->value++;
        
      }
      else{
        allWord++;
        coll++;
        Map * cur;
        for(cur = findMap; cur->nextPtr; cur = cur->nextPtr){}
        cur->nextPtr = (Map *)malloc(sizeof(Map));
        cur = cur->nextPtr;
        memcpy(cur, tMap, sizeof(Map));
        cur->nextPtr = NULL;

      }
      
    }
    i++;
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

  
  pthread_t * threads = (pthread_t *)malloc(0x10 * sizeof(pthread_t));
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
  //printf("%d / %d\n", fileLength, limit);

  while(!feof(fp)){
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
    //printf("%d\n",nWord);
    //printf("complete map\n");
    struct _thread_arg *args[15] = { 0, };
    for(int32_t i = 0; i < nThread; i++){
      args[i] = (struct _thread_arg *)malloc(sizeof(struct _thread_arg));
      args[i]->nWord = nWord;
      args[i]->max = nWord/14;
      args[i]->position = (nWord/14)*i;
      pthread_create(&threads[i], NULL, do_Reduce, args[i]);
    }
    
    if(nWord%14){
      args[14] = (struct _thread_arg *)malloc(sizeof(struct _thread_arg));
      args[14]->nWord = nWord;
      args[14]->max = nWord%14;
      args[14]->position = (nWord/14)*14;
      pthread_create(&threads[14], NULL, do_Reduce, args[14]);
    } 

    for(int32_t i = 0; i < nThread; i++){
      pthread_join(threads[i], NULL);
    }
    
    if(nWord%14)
      pthread_join(threads[14], NULL);
    for(int32_t i = 0; i < 15; i++)
      free(args[i]);
    //printf("complete reduce\n");
    //rintf("%d / %d / %d\n",allWord, coll, dup);
    //printf("%d : 0x%08x 0x%08x\n",i,hi, hi2);
    
  }
  nResult = allWord;
  result = (Map *)mmap(0, sizeof(Map)* nResult, PROT_READ|PROT_WRITE, MAP_PRIVATE | 0x20, -1, 0);
  reduceflat();
  printf("complete : %d\n", reduceN);
  qsort(result, reduceN, sizeof(Map), compare);
  
  for(int32_t i = 0; i < reduceN; i++)
    printf("%s %d\n", result[i].hash.key, result[i].value);	
  printf("complete : %d\n", reduceN);
  /*
  while(fscanf(fp, "%s", buf) != EOF)
    printf("[%s]\n", buf);
  */
}
