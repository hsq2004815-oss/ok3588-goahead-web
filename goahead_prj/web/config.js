window.menu=[
    {name:'首页',key:'home',filePath:'/home',type:1,icon:'HomeFilled'},
    {name:'模型验证',key:'modelVerify',type:0,icon:'Connection',
        children:[
            {name:'图片验证',key:'checkMng',filePath:'/checkMng',type:1,icon:'Picture'},
            {name:'视频检测',key:'videoMng',filePath:'/videoMng',type:1,icon:'VideoCamera'},
            {name:'实时检测',key:'demoMng',filePath:'/demoMng',type:1,icon:'VideoPlay'},
        ]
    },
    {name:'我的模型',key:'mode',type:0,icon:'Collection',
        children:[
            { name:'添加模型',key:'addMode',filePath:'/mode/addMode',type:1},
            { name:'模型管理',key:'modeMng',filePath:'/mode/modeMng',type:1},
            {name:'云端模型',key:'cloudMng',filePath:'/cloudMng',type:1,icon:'CloudDownload'},
        ]
    },
    {name:'系统信息',key:'system',type:0,icon:'Coin',
        children:[
            { name:'设备状态',key:'deviceStatus',filePath:'/system/deviceStatus',type:1},
            { name:'系统日志',key:'log',filePath:'/system/log',type:1},
        ]
    }
]
window.systemName='AI边缘计算管理平台';
window.startText='开始';
window.stopText='暂停';