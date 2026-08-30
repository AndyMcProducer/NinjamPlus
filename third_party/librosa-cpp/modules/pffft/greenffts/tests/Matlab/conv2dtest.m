% program to test 2d real fast conv

% let user select file then open it
fname = 'conv2ddat.c2d';
if (!exist(fname, 'file'))
  [fname, pname] = uigetfile('*.c2d', 'select conv file');
  cd(pname);
end
fidout=fopen(fname,'r');

% read header info
aN=fread(fidout,1,'long');
aM=fread(fidout,1,'long');
bN=fread(fidout,1,'long');
bM=fread(fidout,1,'long');
% read in data
%status=fseek(fidout,Nheader,'bof');
a=fread(fidout,aN*aM,'float');
a=reshape(a,aN,aM);
b=fread(fidout,bN*bM,'float');
b=reshape(b,bN,bM);
c=fread(fidout,(aN+bN-1)*(aM+bM-1),'float');
c=reshape(c,(aN+bN-1),(aM+bM-1));
fclose(fidout);

c2=conv2(a,b);
maxerr = max(max(abs(c2-c)));
printf("======================================================\n");
printf("conv2dtest: maximum error from C to octave: %g\n", maxerr);
printf("======================================================\n");
if (maxerr >= 1E-4)
  printf("ERROR! - see https://stackoverflow.com/questions/57940972/octave-not-returning-exit-code");
  exit(1)
end
