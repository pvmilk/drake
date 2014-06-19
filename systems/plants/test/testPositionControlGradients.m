function testPositionControlGradients

p = TimeSteppingRigidBodyManipulator('ActuatedPendulum.urdf', .01);
p = enableIdealizedPositionControl(p, true);
p = compile(p);

num_tests = 10;

x = 100*rand(2,num_tests);
u = rand(1,num_tests);

for i=1:num_tests
    
    [xdn1,df1] = geval(1,@update,p,0,x(:,i),u(:,i),struct('grad_method','numerical'));
    [xdn2,df2] = geval(1,@update,p,0,x(:,i),u(:,i),struct('grad_method','user'));

    if (any(abs(xdn1-xdn2)>1e-5))
        error('Next states when computing gradients don''t match!');
    end
    if (any(any(abs(df1-df2)>1e-5)))
        error('Gradients don''t match!');
    end

end