#include <stdlib.h>
#include <stdio.h>
#include <direct.h>

// nt_clean_tree.bat

int main(int argc, char ** argv)
{
  int iRetCode;
	char szNMake[50];
	char szCWD[250];
  int i;

  for(i=1; i < argc; i++)
  {

    if(iRetCode = _chdir(argv[i]))
	  {
	    fprintf(stderr, "Error changing directory to %s\n",argv[i]);
		  goto abort;
	  }

    printf("Cleaning %s\n",argv[i]);

    sprintf(szNMake, "nmake clean_nt");

	  
    if(iRetCode = system(szNMake))
	  {
	    fprintf(stderr, "Error executing ((%s))\n",szNMake);
		  goto abort;
	  }

    if(iRetCode = _chdir(".."))
	  {
	    fprintf(stderr, "Error changing directory to %s\n","..");
		  goto abort;
	  }

  }

	fprintf(stderr, "Successfully completed(nt_clean_tree) make in directory: %s\n",_getcwd(szCWD,250));
  return(iRetCode);

  abort:
	
	fprintf(stderr, "Error(nt_clean_tree) during make in directory: %s\n",_getcwd(szCWD,250));
  return(iRetCode);
	
}