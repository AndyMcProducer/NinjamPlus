% program to test 1d real fast conv
clear c2

% let user select file then open it
fname = 'convdat.cnv';
if (!exist(fname, 'file'))
  [fname, pname] = uigetfile('*.cnv', 'select conv file');
  cd(pname);
end

fidout=fopen(fname,'r');

% read header info
aN=fread(fidout,1,'long');
bN=fread(fidout,1,'long');
M=fread(fidout,1,'long');
% read in data
%status=fseek(fidout,Nheader,'bof');
a=fread(fidout,aN*M,'float');
a=reshape(a,aN,M);
b=fread(fidout,bN*M,'float');
b=reshape(b,bN,M);
c=fread(fidout,(aN+bN-1)*M,'float');
c=reshape(c,(aN+bN-1),M);
fclose(fidout);

for i1=1:M;
	c2(:,i1)=conv(a(:,i1),b(:,i1));
end;
maxerr = max(max(abs(c2-c)));
printf("======================================================\n");
printf("convtest: maximum error from C to octave: %g\n", maxerr);
printf("======================================================\n");
if (maxerr >= 1E-5)
  printf("ERROR! - see https://stackoverflow.com/questions/57940972/octave-not-returning-exit-code");
  exit(1)
end
